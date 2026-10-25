#include "OrganEngine.h"

namespace rysum
{
static constexpr float kSpeedOfSound = 343.0f;

OrganEngine::OrganEngine()
{
    voices.resize ((size_t) kMaxVoices);
    noise.resize ((size_t) kMaxNoiseVoices);
    for (int s = 0; s < kNumStops; ++s)
    {
        std::array<Pos, kNumKeys> pos {};
        generateLayout (defaultLayout (s), s, pos);
        for (int k = 0; k < kNumKeys; ++k)
        {
            baseAz[(size_t) s][(size_t) k] = pos[(size_t) k].az;
            baseEl[(size_t) s][(size_t) k] = pos[(size_t) k].el;
            baseDist[(size_t) s][(size_t) k] = pos[(size_t) k].dist;
            muted[(size_t) s][(size_t) k] = false;
        }
    }
    uint32_t seed = 1;
    for (auto& v : voices)
    {
        v.deg.reset (seed++ * 7919u);
        XorShift r (seed * 31u);
        v.wobble.init (r); v.breath.init (r);
        v.breath.f1 *= 6.0f; v.breath.f2 *= 4.0f; v.breath.f3 *= 2.0f;
        v.grainRng = XorShift (seed * 104729u + 17u);
    }
}

void OrganEngine::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate > 0 ? sampleRate : 48000.0;
    maxBlock = juce::jmax (1, maxBlockSize);
    scratch.assign ((size_t) maxBlock, 0.0f);
    scratchRamp.assign ((size_t) maxBlock, 0.0f);
    roomSend.assign ((size_t) maxBlock, 0.0f);
    gust.assign ((size_t) (maxBlock / 16 + 4), 0.0f);
    trem.assign ((size_t) (maxBlock / 16 + 4), 0.0f);
    for (auto& v : voices) v.active = false;
    for (auto& n : noise) n.active = false;
    keyHeld.fill (false);
    keySustained.fill (false);
    firstBlock = true;
}

void OrganEngine::setLibrary (std::shared_ptr<const SampleLibrary> lib)
{
    for (auto& v : voices) v.active = false;
    for (auto& n : noise) n.active = false;
    library = std::move (lib);
    windPos = 0.0;
}

void OrganEngine::requestPreview (int stop, int key)
{
    const auto scope = previewFifo.write (1);
    if (scope.blockSize1 > 0) previewQueue[(size_t) scope.startIndex1] = { stop, key };
    else if (scope.blockSize2 > 0) previewQueue[(size_t) scope.startIndex2] = { stop, key };
}

Pos OrganEngine::currentPosition (int stop, int key, const EngineParams& p) const noexcept
{
    Pos base { baseAz[(size_t) stop][(size_t) key].load(), baseEl[(size_t) stop][(size_t) key].load(),
               baseDist[(size_t) stop][(size_t) key].load() };
    return applyMotion (base, (Motion) p.motionMode[(size_t) stop], motionPhase[(size_t) stop].load(),
                        p.motionDepth[(size_t) stop], (uint32_t) (stop * 101 + key * 7 + 1), p.ellipse[(size_t) stop]);
}

void OrganEngine::gainsFor (Pos pos, float* out) const noexcept
{
    std::fill (out, out + kMaxAmbiChannels, 0.0f);
    if (order == 0) encodeStereo (pos.az, out);
    else            encodeAmbiX (pos.az, pos.el, order, out);
}

/** direction gains, distance level, air absorption, room send and Doppler of one voice for this block */
void OrganEngine::updateVoiceSpatial (Voice& v, const EngineParams& p, double blockSeconds) noexcept
{
    const Pos pos = currentPosition (v.stop, v.key, p);
    gainsFor (pos, v.g1.data());
    const float sg = p.stopGain[(size_t) v.stop];
    const float g = sg * distanceGain (pos.dist, p.distAmount);
    for (int c = 0; c < kMaxAmbiChannels; ++c) v.g1[(size_t) c] *= g;
    v.send1 = sg * distanceGain (pos.dist, p.distAmount * 0.35f);       // the room fades much less with distance

    // air: far pipes lose their highs
    const float fc = juce::jlimit (1200.0f, 20000.0f, 20000.0f / (1.0f + p.air * pos.dist / 5.0f));
    v.airA = fc >= 19999.0f ? 1.0f : 1.0f - std::exp (-juce::MathConstants<float>::twoPi * fc / (float) sr);

    // Doppler from the radial speed (not for teleporting pipes)
    if (v.prevDist < 0.0f || (Motion) p.motionMode[(size_t) v.stop] == Motion::Jumps || p.doppler <= 0.0f || blockSeconds <= 0.0)
        v.dopTarget = 1.0f;
    else
    {
        const float vr = juce::jlimit (-120.0f, 120.0f, (float) ((pos.dist - v.prevDist) / blockSeconds));
        v.dopTarget = juce::jlimit (0.5f, 2.0f, kSpeedOfSound / (kSpeedOfSound + p.doppler * vr));
    }
    v.prevDist = pos.dist;
}

// ---------------------------------------------------------------------------
float OrganEngine::readSample (const float* d, int len, double pos) const noexcept
{
    // 4-point cubic Hermite
    const int i = (int) pos;
    const float f = (float) (pos - i);
    auto at = [d, len] (int j) { return (j >= 0 && j < len) ? d[j] : 0.0f; };
    const float xm1 = at (i - 1), x0 = at (i), x1 = at (i + 1), x2 = at (i + 2);
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}

/** freeze: overlapping grains taken from the sustain of the pipe, slightly detuned */
float OrganEngine::grainSample (Voice& v, double rate, float smear) noexcept
{
    const Pipe& pipe = *v.pipe;
    if (--v.grainTimer <= 0)
    {
        int regionStart = pipe.hasLoop() ? pipe.loopStart : juce::jmin (pipe.attackEnd + (int) (0.1 * kSampleFileRate), pipe.release);
        int regionEnd   = pipe.hasLoop() ? pipe.loopEnd : pipe.release;
        if (regionEnd - regionStart < 4096) { regionStart = juce::jmax (0, pipe.attackEnd); regionEnd = juce::jmax (regionStart + 16, pipe.release); }
        const int glen = (int) ((0.06 + 0.5 * smear) * sr);
        for (auto& g : v.grains)
            if (! g.on)
            {
                const double span = juce::jmax (1.0, (double) (regionEnd - regionStart) - glen * rate * 1.1);
                g.pos = regionStart + v.grainRng.uni() * span;
                g.len = juce::jmax (64, glen);
                g.age = 0;
                g.rateMul = std::exp2 (smear * 25.0f * v.grainRng.bi() / 1200.0f);
                g.on = true;
                break;
            }
        v.grainTimer = juce::jmax (16, (int) ((float) glen / 3.0f * (0.7f + 0.6f * v.grainRng.uni())));
    }
    float s = 0.0f;
    for (auto& g : v.grains)
    {
        if (! g.on) continue;
        const float ph = (float) g.age / (float) g.len;
        const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * ph);
        s += w * readSample (v.data, v.length, g.pos);
        g.pos += rate * g.rateMul;
        if (++g.age >= g.len) g.on = false;
    }
    return s * (1.0f / 1.5f);
}

void OrganEngine::startVoice (int stop, int key, int previewSamples)
{
    if (library == nullptr) return;
    const Pipe& pipe = library->pipes[(size_t) stop][(size_t) key];
    if (! pipe.valid || pipe.sample == nullptr) return;
    if (previewSamples < 0 && muted[(size_t) stop][(size_t) key].load()) return;

    Voice* v = nullptr;
    for (auto& c : voices) if (! c.active) { v = &c; break; }
    if (v == nullptr)
    {
        // steal the oldest releasing voice, else the oldest one
        Voice* oldest = nullptr;
        for (auto& c : voices)
            if (! c.held && (oldest == nullptr || c.age < oldest->age)) oldest = &c;
        if (oldest == nullptr)
            for (auto& c : voices)
                if (oldest == nullptr || c.age < oldest->age) oldest = &c;
        v = oldest;
    }

    v->active = true;
    v->stop = stop; v->key = key;
    v->pipe = &pipe;
    v->data = pipe.sample->data.data();
    v->length = pipe.sample->size();
    v->pos = 0.0;
    v->baseRate = pipe.rate * kSampleFileRate / sr;     // pipe.rate already contains file-rate / 48k
    v->looping = pipe.hasLoop();
    if (v->looping)
    {
        const int loopLen = pipe.loopEnd - pipe.loopStart;
        v->loopXf = juce::jlimit (16, juce::jmax (16, pipe.loopStart), juce::jmin (1440, loopLen / 3));
    }
    v->held = true;
    v->relXfade = false;
    v->killing = false;
    v->killGain = 1.0f;
    v->killStep = (float) (1.0 / (0.005 * sr));
    v->previewRemain = previewSamples;
    v->age = ++ageCounter;
    v->fresh = true;
    v->env = 0.0f; v->held1 = 0.0f;
    v->windSm = -1.0f; v->lp = 0.0f; v->lpA = 1.0f;
    v->lpAir = 0.0f; v->airA = 1.0f;
    v->prevDist = -1.0f; v->dop = 1.0f; v->dopTarget = 1.0f;
    XorShift dr ((uint32_t) (stop * 7919 + key * 104729 + 3));
    dr.next();
    v->detuneRnd = dr.bi();
    v->frozen = false; v->freezeMix = 0.0f; v->grainTimer = 0;
    for (auto& g : v->grains) g.on = false;
}

void OrganEngine::releaseVoice (Voice& v)
{
    if (! v.held) return;
    v.held = false;
    const Pipe& p = *v.pipe;
    const int relLenFile = (int) (0.04 * kSampleFileRate);
    const double target = (double) juce::jmax (0, p.release - relLenFile);
    if (v.pos < target)
    {
        // crossfade from wherever we are (usually inside the loop) to just before the
        // recorded key release, then let the real release tail play
        v.relXfade = true;
        v.relPos = target;
        v.relCount = 0;
        v.relLen = juce::jmax (1, (int) (0.04 * sr));
    }
    else
    {
        v.looping = false;
    }
}

void OrganEngine::freezeVoice (Voice& v)
{
    if (! v.held) return;
    v.held = false;          // no longer belongs to a key
    v.frozen = true;
    v.freezeMix = 0.0f;
    v.grainTimer = 0;
}

void OrganEngine::releaseStopKey (int stop, int key)
{
    for (auto& v : voices)
        if (v.active && v.held && v.stop == stop && v.key == key && v.previewRemain < 0)
            releaseVoice (v);
}

void OrganEngine::playNoise (const std::vector<NoiseSample>& pool, float gain, const float* gains, int& lastIndex, float send)
{
    if (pool.empty() || gain <= 0.0001f) return;
    int idx = (int) (rng.uni() * (float) pool.size());
    if (pool.size() > 1 && idx == lastIndex) idx = (idx + 1) % (int) pool.size();
    idx = juce::jlimit (0, (int) pool.size() - 1, idx);
    lastIndex = idx;
    const auto& s = pool[(size_t) idx].sample;
    NoiseVoice* nv = nullptr;
    for (auto& n : noise) if (! n.active) { nv = &n; break; }
    if (nv == nullptr) nv = &noise[(size_t) (rng.next() % noise.size())];
    nv->active = true;
    nv->data = s->data.data();
    nv->length = s->size();
    nv->pos = 0.0;
    nv->rate = s->sampleRate / sr * (0.97 + 0.06 * rng.uni());     // tiny variation per click
    nv->gain = gain * (0.85f + 0.3f * rng.uni());
    nv->send = send;
    std::copy (gains, gains + kMaxAmbiChannels, nv->g.begin());
}

void OrganEngine::keyDown (int key, float velocity, const EngineParams& p)
{
    keyHeld[(size_t) key] = true;
    keySustained[(size_t) key] = false;

    // click: blend between the console position and the average position of the pipes
    std::array<float, kMaxAmbiChannels> console {}, pipes {}, tmp {};
    gainsFor ({ p.consoleAz, p.consoleEl }, console.data());
    int n = 0;
    for (int s = 0; s < kNumStops; ++s)
    {
        if (! p.stopOn[(size_t) s]) continue;
        gainsFor (currentPosition (s, key, p), tmp.data());
        for (int c = 0; c < kMaxAmbiChannels; ++c) pipes[(size_t) c] += tmp[(size_t) c];
        ++n;
    }
    std::array<float, kMaxAmbiChannels> g {};
    for (int c = 0; c < kMaxAmbiChannels; ++c)
        g[(size_t) c] = n > 0 ? (1.0f - p.clickBlend) * console[(size_t) c] + p.clickBlend * pipes[(size_t) c] / (float) n
                              : console[(size_t) c];
    if (library != nullptr)
        playNoise (library->clicksDown.empty() ? library->clicksOther : library->clicksDown,
                   p.clickLevel * (0.55f + 0.45f * velocity) * 1.6f, g.data(), lastDown, 0.5f);

    for (int s = 0; s < kNumStops; ++s)
        if (p.stopOn[(size_t) s])
        {
            releaseStopKey (s, key);
            startVoice (s, key);
        }
}

void OrganEngine::keyUp (int key, const EngineParams& p)
{
    if (! keyHeld[(size_t) key]) return;
    keyHeld[(size_t) key] = false;
    std::array<float, kMaxAmbiChannels> console {};
    gainsFor ({ p.consoleAz, p.consoleEl }, console.data());
    if (library != nullptr)
        playNoise (library->clicksUp.empty() ? library->clicksOther : library->clicksUp,
                   p.releaseClick * 1.6f, console.data(), lastUp, 0.5f);
    for (auto& v : voices)
        if (v.active && v.held && v.key == key && v.previewRemain < 0)
        {
            if (p.freeze) freezeVoice (v);      // freeze: the pipe keeps sounding as a grain cloud
            else          releaseVoice (v);
        }
}

void OrganEngine::handleMidi (const juce::MidiMessage& m, const EngineParams& p)
{
    if (m.isNoteOn())
    {
        const int k = keyIndexForMidi (m.getNoteNumber());
        if (k >= 0) keyDown (k, m.getFloatVelocity(), p);
    }
    else if (m.isNoteOff())
    {
        const int k = keyIndexForMidi (m.getNoteNumber());
        if (k < 0) return;
        if (sustainPedal) { keySustained[(size_t) k] = true; return; }
        keyUp (k, p);
    }
    else if (m.isSustainPedalOn()) sustainPedal = true;
    else if (m.isSustainPedalOff())
    {
        sustainPedal = false;
        for (int k = 0; k < kNumKeys; ++k)
            if (keySustained[(size_t) k]) { keySustained[(size_t) k] = false; keyUp (k, p); }
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        for (int k = 0; k < kNumKeys; ++k)
            if (keyHeld[(size_t) k]) keyUp (k, p);
        if (m.isAllSoundOff())
            for (auto& v : voices) if (v.active) { v.killing = true; }
    }
}

// ---------------------------------------------------------------------------
void OrganEngine::renderRange (juce::AudioBuffer<float>& out, int start, int end, int blockLen, const EngineParams& p)
{
    const int n = end - start;
    if (n <= 0) return;
    const bool perPipeDeg = p.deg.perPipe && p.deg.amount > 0.0001f;
    const float wobAmt = p.deg.amount * p.deg.wobble;
    // "Windschwankung" gets disproportionately wilder towards 100 %
    const float wobCents = 30.0f * wobAmt + 320.0f * wobAmt * wobAmt * wobAmt * wobAmt;
    const float wobLevel = 0.3f * wobAmt + 0.6f * wobAmt * wobAmt * wobAmt * wobAmt;
    const float wobSpeed = 1.0f + 5.0f * wobAmt * wobAmt * wobAmt;
    // global wind: above ~35 % the wind supply starts to shake – shared by all pipes (same wind chest)
    const float turbLin = juce::jlimit (0.0f, 1.0f, (p.wind - 0.35f) / 0.65f);
    const float turb = turbLin * turbLin;
    const float turbCents = 190.0f * turb, turbAmp = 0.75f * turb;
    const double dt = 1.0 / sr;
    const float twoPiOverSr = (float) (juce::MathConstants<double>::twoPi / sr);
    const float freezeStep = (float) (1.0 / (0.15 * sr));
    const float tremCents = p.tremOn ? 16.0f * p.tremDepth : 0.0f;
    const float tremAmp = p.tremOn ? 0.32f * p.tremDepth : 0.0f;

    // shared curves for this range (one value per 16 samples): wind gusts and tremulant
    const int steps = n / 16 + 2;
    if ((int) gust.size() < steps) { gust.assign ((size_t) steps, 0.0f); trem.assign ((size_t) steps, 0.0f); }
    for (int j = 0; j < steps; ++j)
    {
        gust[(size_t) j] = turb > 0.0f ? smoothNoise (gustPhase + j * 16.0 * dt * (0.5 + 6.0 * turb), 4242u) : 0.0f;
        // a tremulant is a periodic interruption of the wind: slightly asymmetric wave
        const double tp = juce::MathConstants<double>::twoPi * (tremPhase + j * 16.0 * dt * p.tremRate);
        trem[(size_t) j] = p.tremOn ? (float) (std::sin (tp) + 0.25 * std::sin (2.0 * tp + 0.7)) / 1.2f : 0.0f;
    }
    gustPhase += n * dt * (0.5 + 6.0 * turb);
    tremPhase = std::fmod (tremPhase + n * dt * p.tremRate, 1.0);

    float* send = roomSend.data() + start;
    const float inv = 1.0f / (float) blockLen;

    for (auto& v : voices)
    {
        if (! v.active) continue;
        const float* d = v.data;
        const int len = v.length;
        const Pipe& pipe = *v.pipe;
        const int loopLen = pipe.loopEnd - pipe.loopStart;
        float* buf = scratch.data();
        bool finished = false;
        const float windTarget = juce::jlimit (0.0f, 1.0f, p.stopWind[(size_t) v.stop]);
        const int pc = pipe.midi % 12;
        const float tuneCents = p.tuneCents[(size_t) pc] + p.pitchCents + p.detune * v.detuneRnd;

        double rate = v.baseRate;
        float amp = 1.0f;
        for (int i = 0; i < n; ++i)
        {
            if ((i & 15) == 0)       // control rate: every 16 samples
            {
                // stop wind: a half-drawn stop starves the pipe – quieter, flatter, unsteady, duller
                if (v.windSm < 0.0f) v.windSm = windTarget;
                v.windSm += 0.03f * (windTarget - v.windSm);
                const float w = v.windSm, lack = 1.0f - w;
                float cents = tuneCents - 75.0f * lack * std::sqrt (lack);
                float a = std::pow (w, 1.6f);
                if (lack > 0.001f)
                {
                    v.breath.advance (dt * 16.0 * (1.0 + 3.0 * lack));
                    const float br = v.breath.value();
                    cents += 35.0f * lack * lack * br;
                    a *= 1.0f + 0.4f * lack * br;
                }
                v.lpA = w > 0.995f ? 1.0f : 1.0f - std::exp (-twoPiOverSr * (250.0f + 16000.0f * w * w));

                if (wobAmt > 0.0001f)
                {
                    v.wobble.advance (dt * 16.0 * wobSpeed);
                    const float wv = v.wobble.value();
                    cents += wobCents * wv;
                    a *= juce::jmax (0.0f, 1.0f + wobLevel * wv);
                }
                if (turb > 0.0f)
                {
                    const float g = gust[(size_t) (i >> 4)];
                    cents += turbCents * g;
                    a *= juce::jmax (0.05f, 1.0f + turbAmp * g);
                }
                if (tremCents > 0.0f)
                {
                    const float t = trem[(size_t) (i >> 4)];
                    cents += tremCents * t;
                    a *= 1.0f + tremAmp * t;
                }
                v.dop += 0.08f * (v.dopTarget - v.dop);
                rate = v.baseRate * std::exp2 (cents / 1200.0f) * v.dop;
                amp = a;
            }

            float x = 0.0f;
            if (! v.frozen || v.freezeMix < 1.0f)
            {
                if (v.looping && v.pos >= pipe.loopEnd - v.loopXf)
                {
                    const float t = (float) ((v.pos - (pipe.loopEnd - v.loopXf)) / v.loopXf);
                    x = (1.0f - t) * readSample (d, len, v.pos) + t * readSample (d, len, v.pos - loopLen);
                }
                else x = readSample (d, len, v.pos);

                bool jumped = false;
                if (v.relXfade)
                {
                    const float g = (float) v.relCount / (float) v.relLen;
                    x = x * (1.0f - g) + readSample (d, len, v.relPos) * g;
                    v.relPos += rate;
                    if (++v.relCount >= v.relLen)
                    {
                        // crossfade done: continue from the release reader, no more looping
                        v.relXfade = false; v.looping = false; v.pos = v.relPos; jumped = true;
                    }
                }
                if (! jumped)
                {
                    v.pos += rate;
                    if (v.looping && v.pos >= pipe.loopEnd) v.pos -= loopLen;
                }
            }
            if (v.frozen)
            {
                const float gs = grainSample (v, rate, p.freezeSmear);
                if (v.freezeMix < 1.0f) v.freezeMix = juce::jmin (1.0f, v.freezeMix + freezeStep);
                x = x * (1.0f - v.freezeMix) + gs * v.freezeMix;
            }

            if (v.killing)
            {
                v.killGain -= v.killStep;
                if (v.killGain <= 0.0f) { finished = true; x = 0.0f; }
                x *= juce::jmax (0.0f, v.killGain);
            }

            v.lp += v.lpA * (x - v.lp);
            v.lpAir += v.airA * (v.lp - v.lpAir);
            x = v.lpAir * amp;

            if (perPipeDeg)
            {
                const auto f = v.deg.frame (p.deg, sr);
                x = DegradeState::process (x, v.held1, v.env, f, p.deg);
            }

            buf[i] = x;

            if (v.previewRemain > 0 && --v.previewRemain == 0) releaseVoice (v);
            if (! v.frozen && v.pos >= len - 2) { finished = true; for (int j = i + 1; j < n; ++j) buf[j] = 0.0f; break; }
            if (finished) { for (int j = i + 1; j < n; ++j) buf[j] = 0.0f; break; }
        }

        // add to the output with the gain ramp of this block:
        // o += buf * (ga + d*i)  ==  o += ga*buf + d*(buf*i)   (two vectorised passes per channel)
        float* bufRamp = scratchRamp.data();
        for (int i = 0; i < n; ++i) bufRamp[i] = buf[i] * (float) i;
        for (int c = 0; c < usedChannels; ++c)
        {
            const float a = v.g0[(size_t) c], b = v.g1[(size_t) c];
            float* o = out.getWritePointer (c) + start;
            const float ga = a + (b - a) * (float) start * inv;
            const float dg = (b - a) * inv;
            if (std::abs (ga) > 1.0e-9f) juce::FloatVectorOperations::addWithMultiply (o, buf, ga, n);
            if (std::abs (dg) > 1.0e-12f) juce::FloatVectorOperations::addWithMultiply (o, bufRamp, dg, n);
        }
        {
            const float sa = v.send0 + (v.send1 - v.send0) * (float) start * inv;
            const float sd = (v.send1 - v.send0) * inv;
            juce::FloatVectorOperations::addWithMultiply (send, buf, sa, n);
            if (std::abs (sd) > 1.0e-12f) juce::FloatVectorOperations::addWithMultiply (send, bufRamp, sd, n);
        }
        if (finished) v.active = false;
    }

    // noise one-shots
    for (auto& nv : noise)
    {
        if (! nv.active) continue;
        for (int i = 0; i < n; ++i)
        {
            if (nv.pos >= nv.length - 2) { nv.active = false; break; }
            const float x = readSample (nv.data, nv.length, nv.pos) * nv.gain;
            nv.pos += nv.rate;
            for (int c = 0; c < usedChannels; ++c)
                if (std::abs (nv.g[(size_t) c]) > 1.0e-9f) out.getWritePointer (c)[start + i] += x * nv.g[(size_t) c];
            send[i] += x * nv.send;
        }
    }

    // wind: omnidirectional (W) in Ambisonics, centre in stereo
    if (library != nullptr && ! library->wind.empty())
    {
        const auto& w = *library->wind.front();
        const int wl = w.size();
        const float target = p.wind * 2.0f;
        const double wr = w.sampleRate / sr;
        if (wl > 4 && (target > 0.0001f || windGain > 0.0001f))
        {
            for (int i = 0; i < n; ++i)
            {
                windGain += 0.0005f * (target - windGain);
                const float gst = turb > 0.0f ? gust[(size_t) (i >> 4)] : 0.0f;
                const float tr = p.tremOn ? 0.5f * p.tremDepth * trem[(size_t) (i >> 4)] : 0.0f;
                float x = readSample (w.data.data(), wl, windPos) * windGain
                          * juce::jmax (0.0f, 1.0f + 1.6f * turb * gst + 1.5f * turb + tr);
                windPos += wr;
                if (windPos >= wl - 2) windPos -= (wl - 2);
                if (order == 0) { out.getWritePointer (0)[start + i] += x * 0.7071f; if (usedChannels > 1) out.getWritePointer (1)[start + i] += x * 0.7071f; }
                else out.getWritePointer (0)[start + i] += x;
            }
        }
    }
}

void OrganEngine::process (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi, const EngineParams& pIn)
{
    juce::ScopedNoDenormals noDenormals;
    EngineParams p = pIn;                // local copy with derived values
    p.deg.prepareBlock (sr);
    out.clear();
    const int numSamples = out.getNumSamples();
    if (numSamples <= 0) return;
    if ((int) scratch.size() < numSamples)       // safety (should not happen: prepare() sizes them)
    {
        scratch.assign ((size_t) numSamples, 0.0f); scratchRamp.assign ((size_t) numSamples, 0.0f); roomSend.assign ((size_t) numSamples, 0.0f);
    }
    std::fill (roomSend.begin(), roomSend.begin() + numSamples, 0.0f);
    const double blockSeconds = numSamples / sr;

    order = juce::jlimit (0, kMaxOrder, p.outputMode);
    usedChannels = juce::jmin (out.getNumChannels(), order == 0 ? 2 : (order + 1) * (order + 1));

    // ---- stops pulled / pushed
    if (firstBlock) { lastStopOn = p.stopOn; firstBlock = false; }
    for (int s = 0; s < kNumStops; ++s)
    {
        if (p.stopOn[(size_t) s] == lastStopOn[(size_t) s]) continue;
        std::array<float, kMaxAmbiChannels> console {};
        gainsFor ({ p.consoleAz, p.consoleEl }, console.data());
        if (library != nullptr)
            playNoise (library->stopPulls, p.stopPull * (p.stopOn[(size_t) s] ? 1.4f : 1.1f), console.data(), lastPull, 0.5f);
        for (int k = 0; k < kNumKeys; ++k)
        {
            if (p.stopOn[(size_t) s] && keyHeld[(size_t) k]) startVoice (s, k);
            if (! p.stopOn[(size_t) s]) releaseStopKey (s, k);
        }
    }
    lastStopOn = p.stopOn;

    // ---- freeze switched off: frozen pipes fade out
    if (! p.freeze)
    {
        const float step = (float) (1.0 / (juce::jmax (0.05f, p.freezeRelease) * sr));
        for (auto& v : voices)
            if (v.active && v.frozen && ! v.killing) { v.killing = true; v.killStep = step; }
    }

    // ---- previews requested by the UI
    {
        const auto scope = previewFifo.read (previewFifo.getNumReady());
        auto doOne = [this] (int idx) { auto r = previewQueue[(size_t) idx]; startVoice (r.first, r.second, (int) (1.6 * sr)); };
        for (int i = 0; i < scope.blockSize1; ++i) doOne (scope.startIndex1 + i);
        for (int i = 0; i < scope.blockSize2; ++i) doOne (scope.startIndex2 + i);
    }

    // ---- motion phases: speed * time, or locked to the host's musical position
    for (int s = 0; s < kNumStops; ++s)
    {
        double ph;
        if (p.sync && p.playing)
        {
            // speeds count at 120 BPM (a quarter note = 0.5 s): cycles = speed * quarters * 0.5
            const double ppqEnd = p.ppq + blockSeconds * p.bpm / 60.0;
            ph = (double) p.motionRate[(size_t) s] * ppqEnd * 0.5;
        }
        else
        {
            const double scale = p.sync ? p.bpm / 120.0 : 1.0;
            ph = motionPhase[(size_t) s].load() + (double) p.motionRate[(size_t) s] * scale * blockSeconds;
        }
        if (std::abs (ph) > 1.0e7) ph = std::fmod (ph, 1000.0);
        motionPhase[(size_t) s] = ph;
    }

    // ---- spatial data for this block (end-of-block targets, ramped from the previous ones)
    for (auto& v : voices)
    {
        if (! v.active) continue;
        if (! v.fresh) { v.g0 = v.g1; v.send0 = v.send1; }
        updateVoiceSpatial (v, p, blockSeconds);
        if (v.fresh) { v.g0 = v.g1; v.send0 = v.send1; v.fresh = false; }
    }
    auto initFresh = [&] ()
    {
        for (auto& v : voices)
            if (v.active && v.fresh)
            {
                updateVoiceSpatial (v, p, blockSeconds);
                v.g0 = v.g1; v.send0 = v.send1; v.fresh = false;
                v.dop = v.dopTarget;
            }
    };

    // ---- render, split at MIDI events
    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (0, numSamples, meta.samplePosition);
        renderRange (out, pos, at, numSamples, p);
        pos = at;
        handleMidi (meta.getMessage(), p);
        initFresh();            // voices started mid-block take the end-of-block values right away
    }
    initFresh();                // voices started by stop changes / previews before any MIDI
    renderRange (out, pos, numSamples, numSamples, p);

    // ---- degradation on the sum
    if (! p.deg.perPipe && p.deg.amount > 0.0001f)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const auto f = sumDeg.frame (p.deg, sr);
            for (int c = 0; c < usedChannels; ++c)
            {
                float* o = out.getWritePointer (c);
                o[i] = DegradeState::process (o[i], sumHeld[(size_t) c], sumEnv[(size_t) c], f, p.deg);
            }
        }
    }

    // ---- master gain (smoothed) + meters
    float pk0 = 0.0f, pk1 = 0.0f;
    const float step = (p.masterGain - masterSmoothed) / (float) numSamples;
    for (int c = 0; c < usedChannels; ++c)
    {
        float g = masterSmoothed;
        float* o = out.getWritePointer (c);
        for (int i = 0; i < numSamples; ++i) { g += step; o[i] *= g; }
    }
    {
        float g = masterSmoothed;
        for (int i = 0; i < numSamples; ++i) { g += step; roomSend[(size_t) i] *= g; }
    }
    masterSmoothed = p.masterGain;
    if (out.getNumChannels() > 0) pk0 = out.getMagnitude (0, 0, numSamples);
    if (out.getNumChannels() > 1) pk1 = order == 0 ? out.getMagnitude (1, 0, numSamples) : pk0;
    meterL = juce::jmax (pk0, meterL.load() * 0.9f);
    meterR = juce::jmax (pk1, meterR.load() * 0.9f);

    // ---- bookkeeping for the UI
    int count = 0, frozen = 0;
    std::array<uint64_t, kNumStops> sounding {};
    for (auto& v : voices)
        if (v.active) { ++count; frozen += v.frozen ? 1 : 0; sounding[(size_t) v.stop] |= (uint64_t) 1 << v.key; }
    for (int s = 0; s < kNumStops; ++s) soundingMask[(size_t) s] = sounding[(size_t) s];
    activeVoices = count;
    frozenVoices = frozen;
    uint64_t mask = 0;
    for (int k = 0; k < kNumKeys; ++k) if (keyHeld[(size_t) k] || keySustained[(size_t) k]) mask |= (uint64_t) 1 << k;
    heldMask = mask;
    timeSeconds = timeSeconds.load() + blockSeconds;
}
} // namespace rysum
