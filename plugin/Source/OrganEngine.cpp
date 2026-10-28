#include "OrganEngine.h"

namespace rysum
{
static constexpr float kSpeedOfSound = 343.0f;
static constexpr float kChaosLevel = 0.009f;     // wind model 2: level of the whistles / hiss against the pipe samples
static constexpr float kFdnLevel = 6.03f;      // late reverb FDN: level against the convolution tail (same room, same energy)

OrganEngine::OrganEngine()
{
    voices.resize ((size_t) kMaxVoices);
    noise.resize ((size_t) kMaxNoiseVoices);
    for (int k = 0; k < kNumKeys; ++k) { keySource[(size_t) k] = k; slotHz[(size_t) k] = (float) meantoneHz (kPhysKeys[(size_t) k]); }
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
    for (auto& r : roomSend) r.assign ((size_t) maxBlock, 0.0f);
    gust.assign ((size_t) (maxBlock / 16 + 4), 0.0f);
    trem.assign ((size_t) (maxBlock / 16 + 4), 0.0f);
    chunkMidi.ensureSize (8192);
    for (auto& v : voices) v.active = false;
    for (auto& n : noise) n.active = false;
    // effects
    echoBuf.assign ((size_t) (16.5 * sr) + (size_t) maxBlock, 0.0f);
    echoWrite = 0; echoDelay = -1.0; echoLp.fill (0.0f);
    fxScratch.assign ((size_t) maxBlock * 12, 0.0f);
    // simulated room (all memory here, nothing is allocated while playing)
    erSlots.assign ((size_t) kErSlots, ErSlot {});
    erRing.assign ((size_t) kErSlots * (size_t) kErRing, 0.0f);
    for (auto& v : voices) v.erSlot = -1;
    for (auto& n : noise) n.erSlot = -1;
    tail.prepare (sr);
    tailGain = 0.0f; tailWasOn = false;
    zeroIn.assign ((size_t) juce::jmax (maxBlock, kSubBlock) + 16, 0.0f);
    fdnBuf.assign ((size_t) kFdnLines * (size_t) kFdnLen, 0.0f);
    fdnPre.assign ((size_t) kFdnLen, 0.0f);
    resetFdn();
    convRingOut = fdnRingOut = 0;
    {
        // FDN mixing matrix: a fixed random orthogonal matrix (Gram-Schmidt) instead of a Hadamard matrix (that one
        // left isolated modes around 1.5-2 kHz). Together with a slow, small modulation of every delay (different
        // rate per line) the modes are smeared out.
        XorShift r (3);
        std::array<std::array<double, kFdnLines>, kFdnLines> m {};
        for (auto& row : m) for (auto& x : row) x = r.bi();
        for (int i = 0; i < kFdnLines; ++i)
        {
            for (int j = 0; j < i; ++j)
            {
                double dot = 0; for (int k = 0; k < kFdnLines; ++k) dot += m[(size_t) i][(size_t) k] * m[(size_t) j][(size_t) k];
                for (int k = 0; k < kFdnLines; ++k) m[(size_t) i][(size_t) k] -= dot * m[(size_t) j][(size_t) k];
            }
            double len = 0; for (int k = 0; k < kFdnLines; ++k) len += m[(size_t) i][(size_t) k] * m[(size_t) i][(size_t) k];
            len = std::sqrt (len);
            for (int k = 0; k < kFdnLines; ++k) m[(size_t) i][(size_t) k] /= len;
        }
        for (int i = 0; i < kFdnLines; ++i) for (int k = 0; k < kFdnLines; ++k) fdnMatrix[(size_t) i][(size_t) k] = (float) m[(size_t) i][(size_t) k];
        for (int j = 0; j < kFdnLines; ++j)
        {
            const double rate = 0.3 + 0.9 * r.uni(), ph = juce::MathConstants<double>::twoPi * r.uni();
            fdnModC[(size_t) j] = (float) std::cos (ph); fdnModS[(size_t) j] = (float) std::sin (ph);
            const double w = juce::MathConstants<double>::twoPi * rate / sr;
            fdnModStepC[(size_t) j] = (float) std::cos (w); fdnModStepS[(size_t) j] = (float) std::sin (w);
        }
    }
    numScheduled = 0;
    {
        XorShift r (77);
        for (size_t i = 0; i < leaks.size(); ++i)
        {
            auto& L = leaks[i];
            L.pos = { 180.0f * r.bi(), -5.0f + 50.0f * r.uni(), 3.0f + 6.0f * r.uni() };
            L.freqMul = 0.7f + 0.7f * r.uni();
            L.dir = r.uni() < 0.5f ? -1.0f : 1.0f;
            L.rng = XorShift ((uint32_t) (i * 7919 + 13));
            L.s1 = L.s2 = L.amp = 0.0f;
        }
    }
    keyHeld.fill (false);
    keySustained.fill (false);
    firstBlock = true;
}

void OrganEngine::setLibrary (std::shared_ptr<const SampleLibrary> lib)
{
    for (auto& v : voices) { v.active = false; releaseSlot (v.erSlot); }
    for (auto& n : noise) { n.active = false; releaseSlot (n.erSlot); }
    library = std::move (lib);
    windPos = 0.0;
    numScheduled = 0;          // (audio thread: no allocation here)
}

// ---------------------------------------------------------------------------
// effects: placed sounds, tracker path, scheduling
// ---------------------------------------------------------------------------
const std::vector<NoiseSample>& OrganEngine::customPool() const noexcept
{
    // no own sounds: the recorded clicks (the "other" ones are the most varied)
    if (library == nullptr) return noSounds;
    if (! library->custom.empty()) return library->custom;
    if (! library->clicksDown.empty()) return library->clicksDown;
    if (! library->clicksOther.empty()) return library->clicksOther;
    return library->stopPulls;
}

void OrganEngine::placedGains (NoiseVoice& nv) noexcept
{
    gainsFor (nv.where, nv.g.data());
    const float dg = distanceGain (nv.where.dist, distAmountNow);
    for (auto& x : nv.g) x *= dg;
    const double a = juce::degreesToRadians ((double) (roomFollowNow ? nv.where.az : roomBaseAzNow));
    nv.sendW[0] = 1.0f;
    for (int m = 1; 2 * m < kMaxRoomSends; ++m)
    {
        nv.sendW[(size_t) (2 * m - 1)] = (float) std::cos (m * a);
        nv.sendW[(size_t) (2 * m)] = (float) std::sin (m * a);
    }
}

void OrganEngine::spawnSound (const Sample& s, float gain, float tone, double rate, Pos pos, float azVel, float send)
{
    if (gain <= 0.00001f || s.size() < 4) return;
    NoiseVoice* nv = nullptr;
    for (auto& n : noise) if (! n.active) { nv = &n; break; }
    if (nv == nullptr) nv = &noise[(size_t) (rng.next() % noise.size())];
    if (nv->active) releaseSlot (nv->erSlot);
    nv->active = true;
    nv->erSlot = roomOnNow ? allocSlot (-1, (int) (nv - noise.data())) : -1;
    nv->data = s.data.data();
    nv->length = s.size();
    nv->pos = 0.0;
    nv->where = pos;
    nv->rate = s.sampleRate / sr * rate;
    nv->gain = gain;
    nv->send = send;
    nv->lp = 0.0f;
    nv->lpA = juce::jlimit (0.02f, 1.0f, tone);
    nv->azVel = azVel;
    nv->where.dist = juce::jlimit (kMinDist, kMaxDist, pos.dist);
    placedGains (*nv);
}

void OrganEngine::schedule (const Scheduled& e)
{
    if (numScheduled < (int) scheduled.size()) scheduled[(size_t) numScheduled++] = e;
}

void OrganEngine::customSounds (int key, float velocity, const EngineParams& p)
{
    const auto& pool = customPool();
    const auto& fx = p.fx;
    if (pool.empty() || fx.custLevel <= 0.0001f) return;
    const int copies = 1 + (int) std::floor (fx.custScatter * (1.0f + 4.0f * rng.uni()));
    std::array<int, kNumStops> pulled {};
    int nPulled = 0;
    for (int s = 0; s < kNumStops; ++s) if (p.stopOn[(size_t) s]) pulled[(size_t) nPulled++] = s;
    for (int c = 0; c < copies; ++c)
    {
        int idx = (int) (rng.uni() * (float) pool.size());
        if (pool.size() > 1 && idx == lastCustom) idx = (idx + 1) % (int) pool.size();
        idx = juce::jlimit (0, (int) pool.size() - 1, idx);
        lastCustom = idx;
        const auto* smp = pool[(size_t) idx].sample.get();
        if (smp == nullptr) continue;
        const float norm = pool[(size_t) idx].norm;

        Pos pos { p.consoleAz, p.consoleEl, 2.0f };
        switch (fx.custPlace)
        {
            case 0:     // at the pipe of a pulled stop (or the console when none is pulled)
                if (nPulled > 0) pos = currentPosition (pulled[(size_t) (rng.next() % (uint32_t) nPulled)], key, p);
                pos.az = wrapAz (pos.az + 35.0f * fx.custScatter * rng.bi());
                break;
            case 1: pos = { 180.0f * rng.bi(), -10.0f + 70.0f * rng.uni(), 2.5f + 9.0f * rng.uni() }; break;
            case 2: break;
            default: pos = { 180.0f * rng.bi(), 30.0f * rng.uni(), 4.0f + 3.0f * rng.uni() }; break;
        }
        float azVel = fx.custWander * (rng.uni() < 0.5f ? -1.0f : 1.0f) * (0.6f + 0.8f * rng.uni());
        if (fx.custPlace == 3 && std::abs (azVel) < 1.0f) azVel = (rng.uni() < 0.5f ? -1.0f : 1.0f) * 90.0f;
        const double rate = std::exp2 ((double) (kPhysKeys[(size_t) key] - 60) / 12.0 * fx.custPitch) * (0.97 + 0.06 * rng.uni());
        const float gain = fx.custLevel * norm * clickCurve (velocity) * 0.7f * (c == 0 ? 1.0f : 0.45f + 0.45f * rng.uni());
        const int delay = c == 0 ? 0 : (int) (rng.uni() * (0.05 + 0.75 * fx.custScatter) * sr);
        if (delay <= 0) spawnSound (*smp, gain, clickTone (velocity), rate, pos, azVel, 0.6f);
        else
        {
            Scheduled e; e.delay = delay; e.sample = smp; e.gain = gain; e.tone = clickTone (velocity);
            e.rate = rate; e.pos = pos; e.azVel = azVel; e.send = 0.6f;
            schedule (e);
        }
    }
}

void OrganEngine::trackerPath (int key, float velocity, const EngineParams& p)
{
    const auto& fx = p.fx;
    if (fx.pathLevel <= 0.0001f || library == nullptr) return;
    const auto& pool = ! library->clicksDown.empty() ? library->clicksDown : library->clicksOther;
    if (pool.empty()) return;
    std::array<int, kNumStops> pulled {};
    int nPulled = 0;
    for (int s = 0; s < kNumStops; ++s) if (p.stopOn[(size_t) s]) pulled[(size_t) nPulled++] = s;
    const int targets = juce::jmin (3, juce::jmax (1, nPulled));
    const int ticks = juce::jlimit (2, 16, fx.pathTicks);
    const Pos console { p.consoleAz, p.consoleEl, 2.0f };
    for (int t = 0; t < targets; ++t)
    {
        const Pos dst = nPulled > 0 ? currentPosition (pulled[(size_t) ((t * nPulled) / targets)], key, p) : Pos { 0.0f, 20.0f, 6.0f };
        const float daz = wrapAz (dst.az - console.az);
        for (int i = 1; i <= ticks; ++i)
        {
            const float f = (float) i / (float) ticks;
            const float bulge = std::sin (juce::MathConstants<float>::pi * f);
            Pos q { wrapAz (console.az + daz * f + fx.pathSpread * bulge * rng.bi()),
                    juce::jlimit (-90.0f, 90.0f, console.el + (dst.el - console.el) * f + 0.5f * fx.pathSpread * bulge * rng.bi()),
                    console.dist + (dst.dist - console.dist) * f };
            const auto& ns = pool[(size_t) (rng.next() % (uint32_t) pool.size())];
            const auto& smp = *ns.sample;
            const float gain = fx.pathLevel * ns.norm * clickCurve (velocity) * 0.25f * (0.45f + 0.55f * f) / std::sqrt ((float) targets);
            const double rate = 1.7 + 1.5 * rng.uni();        // small, high ticks
            const int delay = (int) (fx.pathTime * sr * f * (0.85f + 0.3f * rng.uni()));
            if (delay <= 0) spawnSound (smp, gain, clickTone (velocity), rate, q, 0.0f, 0.3f);
            else
            {
                Scheduled e; e.delay = delay; e.sample = &smp; e.gain = gain; e.tone = clickTone (velocity);
                e.rate = rate; e.pos = q; e.send = 0.3f;
                schedule (e);
            }
        }
    }
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
    // per-pipe envelopes
    for (int j = 0; j < kNumEnvs; ++j)
    {
        auto& e = v.menv[(size_t) j];
        if (! v.held && e.gate) e.releaseGate();
        e.advance (p.mod.env[(size_t) j], (float) blockSeconds);
    }
    // per-pipe modulation targets
    std::array<float, kNumPipeTargets> pm {};
    bool anyMod = false;
    for (const auto& r : p.mod.routes)
        if (r.src >= 0 && r.target >= 0 && r.target < kNumPipeTargets && r.amount != 0.0f)
        {
            pm[(size_t) r.target] += r.amount * shapeSource (r.src, voiceSource (v, r.src, p), r.uni);
            anyMod = true;
        }

    Pos pos = currentPosition (v.stop, v.key, p);
    if (anyMod)
    {
        pos.az = wrapAz (pos.az + 180.0f * pm[tgtAz]);
        pos.el = juce::jlimit (-90.0f, 90.0f, pos.el + 90.0f * pm[tgtEl]);
        const float dm = std::exp2 (3.0f * juce::jlimit (-2.0f, 2.0f, pm[tgtDist]));
        pos.dist = juce::jlimit (kMinDist, kMaxDist, pos.dist * dm);
    }
    modAz[(size_t) v.stop][(size_t) v.key] = 180.0f * pm[tgtAz];
    modEl[(size_t) v.stop][(size_t) v.key] = 90.0f * pm[tgtEl];
    modDistMul[(size_t) v.stop][(size_t) v.key] = std::exp2 (3.0f * juce::jlimit (-2.0f, 2.0f, pm[tgtDist]));
    v.modCents1 = 1200.0f * juce::jlimit (-4.0f, 4.0f, pm[tgtPitch]);

    gainsFor (pos, v.g1.data());
    const float sg = p.stopGain[(size_t) v.stop];
    const float lvl = anyMod ? dbToGain (juce::jlimit (-100.0f, 12.0f, 36.0f * pm[tgtLevel])) : 1.0f;
    const float g = sg * lvl * distanceGain (pos.dist, p.distAmount);
    for (int c = 0; c < kMaxAmbiChannels; ++c) v.g1[(size_t) c] *= g;
    if (p.fx.partSpread > 0.5f)
    {
        Pos ph = pos;
        ph.az = wrapAz (pos.az + p.fx.partSpread * v.partSide + (float) partRot);
        gainsFor (ph, v.gh1.data());
        for (int c = 0; c < kMaxAmbiChannels; ++c) v.gh1[(size_t) c] *= g;
    }
    else v.gh1 = v.g1;
    v.crkEnv = v.held ? 1.0f : v.crkEnv * (float) std::exp (-blockSeconds / 0.15);
    v.send1 = sg * lvl * distanceGain (pos.dist, p.distAmount * 0.35f);       // the room fades much less with distance
    v.erBase = sg * lvl;
    v.erPos = pos;
    v.sendAz1 = p.roomFollow ? pos.az : p.roomBaseAz;
    if (anyMod && pm[tgtSend] != 0.0f) v.send1 *= dbToGain (juce::jlimit (-100.0f, 24.0f, 24.0f * pm[tgtSend]));

    // jumps: a ~4 ms glide so the teleport is sudden but does not click
    if (! v.fresh && (Motion) p.motionMode[(size_t) v.stop] == Motion::Jumps && blockSeconds > 0.0)
    {
        const float k = 1.0f - (float) std::exp (-blockSeconds / 0.004);
        for (int c = 0; c < kMaxAmbiChannels; ++c) v.g1[(size_t) c] = v.g0[(size_t) c] + k * (v.g1[(size_t) c] - v.g0[(size_t) c]);
    }

    // air: far pipes lose their highs
    float fc = juce::jlimit (1200.0f, 20000.0f, 20000.0f / (1.0f + p.air * pos.dist / 5.0f));
    if (anyMod && pm[tgtBright] != 0.0f) fc = juce::jlimit (40.0f, 20000.0f, fc * std::exp2 (6.0f * pm[tgtBright]));
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

void OrganEngine::startVoice (int stop, int key, int previewSamples, int noteId)
{
    if (library == nullptr) return;
    const int src = noteId >= 0 ? key : juce::jlimit (0, kNumKeys - 1, keySource[(size_t) key]);
    const Pipe& pipe = library->pipes[(size_t) stop][(size_t) src];
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

    if (v->active) releaseSlot (v->erSlot);
    v->active = true;
    v->erSlot = roomOnNow ? allocSlot ((int) (v - voices.data()), -1) : -1;
    if (previewSamples < 0) { lastStruckStop = stop; lastStruckKey = key; }
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
    v->chaos.reset ((uint32_t) (stop * 7919 + key * 104729) ^ (ageCounter * 2654435761u), (uint32_t) (stop * 131 + key + 1),
                    (float) (pipe.measuredHz > 0.0 ? pipe.measuredHz : pipe.nominalHz), (float) sr);
    v->chAir = 0.0f;
    v->prevDist = -1.0f; v->dop = 1.0f; v->dopTarget = 1.0f;
    XorShift dr ((uint32_t) (stop * 7919 + key * 104729 + 3));
    dr.next();
    v->detuneRnd = dr.bi();
    v->frozen = false; v->freezeMix = 0.0f; v->grainTimer = 0;
    for (auto& g : v->grains) g.on = false;
    for (auto& e : v->menv) { e.reset(); e.trigger(); }
    v->keyPos = (float) key / (float) (kNumKeys - 1) * 2.0f - 1.0f;
    v->randMod = lastRandom;
    v->noteId = noteId;
    v->noteCents = noteId >= 0 ? centsFor (noteId, key) : 0.0f;
    v->velMod = keyVelocity[(size_t) key];       // (a stop pulled later takes the velocity the key was struck with)
    v->modCents0 = v->modCents1 = 0.0f;
    v->crkAmp = 0.0f; v->crkEnv = 1.0f; v->crkCount = 1 + (int) (rng.uni() * 0.2f * (float) sr);
    v->partLp = 0.0f;
    v->partSide = (hash01 ((uint32_t) (stop * 977 + key * 31 + 5)) < 0.5f ? -1.0f : 1.0f) * (0.6f + 0.4f * hash01 ((uint32_t) (stop * 13 + key * 101 + 9)));
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

void OrganEngine::releaseStopKey (int stop, int key, int noteId)
{
    for (auto& v : voices)
        if (v.active && v.held && v.stop == stop && v.key == key && v.previewRemain < 0 && (noteId < 0 || v.noteId == noteId))
            releaseVoice (v);
}

// ---------------------------------------------------------------------------
// free tuning: every MIDI note its own pitch (Scala file, MTS-ESP)
// ---------------------------------------------------------------------------
float OrganEngine::centsFor (int noteId, int key) const noexcept
{
    if (notesNow == nullptr || noteId < 0 || noteId >= NoteTable::kSize) return 0.0f;
    const float hz = notesNow->hz[(size_t) noteId];
    if (! (hz > 1.0f)) return 0.0f;
    return 1200.0f * std::log2 (hz / slotHz[(size_t) key]);
}

void OrganEngine::noteOnFree (const juce::MidiMessage& m, const EngineParams& p)
{
    const int id = (juce::jlimit (1, 16, m.getChannel()) - 1) * 128 + m.getNoteNumber();
    if (notesNow == nullptr || ! notesNow->mapped[(size_t) id]) return;
    const float hz = notesNow->hz[(size_t) id];
    if (! (hz > 1.0f)) return;
    // the pipe nearest to the wanted pitch plays (beyond the organ's compass: shifted, up to two octaves)
    int k = 0; float best = 1.0e9f;
    for (int i = 0; i < kNumKeys; ++i)
    {
        const float c = std::abs (std::log2 (hz / slotHz[(size_t) i]));
        if (c < best) { best = c; k = i; }
    }
    if (best * 1200.0f > 2400.0f) return;
    auto& fn = freeNotes[(size_t) id];
    if (fn.held) keyUp (fn.key, p, id);                // struck again without a note-off
    fn.held = true; fn.sustained = false; fn.key = k;
    ++keyNoteCount[(size_t) k];
    keyDown (k, m.getFloatVelocity(), p, id);
}

void OrganEngine::noteOffFree (const juce::MidiMessage& m, const EngineParams& p)
{
    const int id = (juce::jlimit (1, 16, m.getChannel()) - 1) * 128 + m.getNoteNumber();
    auto& fn = freeNotes[(size_t) id];
    if (! fn.held) return;
    if (sustainPedal) { fn.sustained = true; return; }
    keyUp (fn.key, p, id);
}

void OrganEngine::releaseAllKeys (const EngineParams& p)
{
    for (int id = 0; id < NoteTable::kSize; ++id)
        if (freeNotes[(size_t) id].held) keyUp (freeNotes[(size_t) id].key, p, id);
    for (int k = 0; k < kNumKeys; ++k)
    {
        keySustained[(size_t) k] = false;
        if (keyHeld[(size_t) k]) keyUp (k, p);
        keyNoteCount[(size_t) k] = 0;
    }
}

void OrganEngine::playNoise (const std::vector<NoiseSample>& pool, float gain, const float* gains, int& lastIndex, float send, float tone)
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
    if (nv->active) releaseSlot (nv->erSlot);
    nv->active = true;
    nv->where = { consoleAzNow, consoleElNow, 2.0f };      // reflections: from the console
    nv->erSlot = roomOnNow ? allocSlot (-1, (int) (nv - noise.data())) : -1;
    nv->data = s->data.data();
    nv->length = s->size();
    nv->pos = 0.0;
    nv->rate = s->sampleRate / sr * (0.97 + 0.06 * rng.uni());     // tiny variation per click
    nv->gain = gain * (0.85f + 0.3f * rng.uni());
    nv->send = send;
    nv->lp = 0.0f;
    nv->lpA = juce::jlimit (0.02f, 1.0f, tone);
    nv->azVel = 0.0f;
    std::copy (gains, gains + kMaxAmbiChannels, nv->g.begin());
    nv->sendW = noiseRoomW;
}

void OrganEngine::keyDown (int key, float velocity, const EngineParams& p, int noteId)
{
    keyHeld[(size_t) key] = true;
    keySustained[(size_t) key] = false;
    globalTrig = true;
    lastKeyPos = (float) key / (float) (kNumKeys - 1) * 2.0f - 1.0f;
    lastRandom = rng.bi();
    lastVelocity = juce::jlimit (0.0f, 1.0f, velocity);
    keyVelocity[(size_t) key] = lastVelocity;

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
    {
        // velocity: a light touch gives a quieter and duller click
        const float v = juce::jlimit (0.0f, 1.0f, velocity);
        keyVelocity[(size_t) key] = v;
        playNoise (library->clicksDown.empty() ? library->clicksOther : library->clicksDown,
                   p.clickLevel * clickCurve (v) * 1.6f, g.data(), lastDown, 0.5f, clickTone (v));
    }

    // effects: own sounds, ticks running to the pipes
    if (p.fx.custLevel > 0.0001f && rng.uni() < p.fx.custDown) customSounds (key, velocity, p);
    trackerPath (key, velocity, p);
    const bool wait = p.fx.pathWait && p.fx.pathLevel > 0.0001f && p.fx.pathTime > 0.005f;

    for (int s = 0; s < kNumStops; ++s)
        if (p.stopOn[(size_t) s])
        {
            releaseStopKey (s, key, noteId);
            if (wait)
            {
                Scheduled e; e.voice = true; e.stop = s; e.key = key; e.noteId = noteId; e.delay = (int) (p.fx.pathTime * sr);
                schedule (e);
            }
            else startVoice (s, key, -1, noteId);
        }
}

void OrganEngine::keyUp (int key, const EngineParams& p, int noteId)
{
    if (noteId >= 0)
    {
        // free tuning: this note lets go; the pipe stays "held" while other notes still sound on it
        auto& fn = freeNotes[(size_t) noteId];
        if (! fn.held) return;
        fn.held = false; fn.sustained = false;
        keyNoteCount[(size_t) key] = juce::jmax (0, keyNoteCount[(size_t) key] - 1);
        keyHeld[(size_t) key] = keyNoteCount[(size_t) key] > 0;
    }
    else
    {
        if (! keyHeld[(size_t) key]) return;
        keyHeld[(size_t) key] = false;
    }
    std::array<float, kMaxAmbiChannels> console {};
    gainsFor ({ p.consoleAz, p.consoleEl }, console.data());
    if (library != nullptr)
    {
        const float v = keyVelocity[(size_t) key];      // the key comes up about as hard as it went down
        playNoise (library->clicksUp.empty() ? library->clicksOther : library->clicksUp,
                   p.releaseClick * clickCurve (v) * 1.6f, console.data(), lastUp, 0.5f, clickTone (v));
    }
    if (p.fx.custLevel > 0.0001f && rng.uni() < p.fx.custUp) customSounds (key, 0.8f * keyVelocity[(size_t) key], p);
    for (auto& v : voices)
        if (v.active && v.held && v.key == key && v.previewRemain < 0 && (noteId < 0 || v.noteId == noteId))
        {
            if (p.freeze) freezeVoice (v);      // freeze: the pipe keeps sounding as a grain cloud
            else          releaseVoice (v);
        }
}

void OrganEngine::handleMidi (const juce::MidiMessage& m, const EngineParams& p)
{
    if (m.isNoteOn())
    {
        if (notesNow != nullptr) { noteOnFree (m, p); return; }
        const int k = keyIndexForMidi (m.getNoteNumber());
        if (k >= 0) keyDown (k, m.getFloatVelocity(), p);
    }
    else if (m.isNoteOff())
    {
        if (notesNow != nullptr) { noteOffFree (m, p); return; }
        const int k = keyIndexForMidi (m.getNoteNumber());
        if (k < 0) return;
        if (sustainPedal) { keySustained[(size_t) k] = true; return; }
        keyUp (k, p);
    }
    else if (m.isController() && m.getControllerNumber() == 1) modWheel = (float) m.getControllerValue() / 127.0f;
    else if (m.isPitchWheel()) pitchWheel = juce::jlimit (-1.0f, 1.0f, (float) (m.getPitchWheelValue() - 8192) / 8191.0f);
    else if (m.isSustainPedalOn()) sustainPedal = true;
    else if (m.isSustainPedalOff())
    {
        sustainPedal = false;
        for (int k = 0; k < kNumKeys; ++k)
            if (keySustained[(size_t) k]) { keySustained[(size_t) k] = false; keyUp (k, p); }
        for (int id = 0; id < NoteTable::kSize; ++id)
            if (freeNotes[(size_t) id].held && freeNotes[(size_t) id].sustained) keyUp (freeNotes[(size_t) id].key, p, id);
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        for (int id = 0; id < NoteTable::kSize; ++id)
            if (freeNotes[(size_t) id].held) keyUp (freeNotes[(size_t) id].key, p, id);
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
        const float tuneCents = (v.noteId >= 0 ? v.noteCents : p.tuning.cents[(size_t) v.key]) + p.pitchCents + p.detune * v.detuneRnd;

        double rate = v.baseRate;
        float amp = 1.0f;
        // effects: upper partials split off (sent elsewhere), crackle bursts at the pipe
        const bool split = p.fx.partSpread > 0.5f;
        float* bufH = fxScratch.data();
        const float splitA = split ? 1.0f - std::exp (-twoPiOverSr * juce::jmin (0.45f * (float) sr,
                                         (float) juce::jmax (20.0, pipe.nominalHz) * p.fx.partSplit)) : 1.0f;
        const bool crackle = p.fx.crackLevel > 0.0001f && v.crkEnv > 0.001f;
        const float crkDecay = std::exp (-1.0f / juce::jmax (1.0f, p.fx.crackLen * 0.001f * (float) sr));
        const float crkMean = (float) sr / juce::jmax (0.05f, p.fx.crackRate);
        const float crkGain = 0.25f * p.fx.crackLevel * v.crkEnv;
        const bool chaosModel = p.windModel == 1;
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
                cents += v.modCents0 + (v.modCents1 - v.modCents0) * (float) (start + i) * inv;
                rate = v.baseRate * std::exp2 (cents / 1200.0f) * v.dop;
                if (chaosModel)
                {
                    // wind model 2: the starving pipe's edge tone (steady at steady wind, lower and quieter as it falls)
                    const double hz = pipe.measuredHz > 0.0 ? pipe.measuredHz : pipe.nominalHz;
                    v.chaos.control (w, (float) (hz * rate / v.baseRate),
                                     v.stop == 1, v.stop == 6, v.held || v.frozen, (float) sr);
                    a *= v.chaos.toneGain();
                }
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
            if (chaosModel && ! v.chaos.idle)
            {
                v.chAir += v.airA * (kChaosLevel * v.chaos.sample() - v.chAir);
                x += v.chAir * (v.killing ? juce::jmax (0.0f, v.killGain) : 1.0f);
            }

            if (perPipeDeg)
            {
                const auto f = v.deg.frame (p.deg, sr);
                x = DegradeState::process (x, v.held1, v.env, f, p.deg);
            }

            if (split)
            {
                v.partLp += splitA * (x - v.partLp);
                bufH[i] = x - v.partLp;
                x = v.partLp;
            }
            if (crackle)
            {
                if (--v.crkCount <= 0)
                {
                    const float u = juce::jmax (1.0e-4f, v.grainRng.uni());
                    v.crkCount = 1 + (int) (-std::log (u) * crkMean);
                    const float r = v.grainRng.uni();
                    v.crkAmp = 0.2f + 0.8f * r * r;
                }
                if (v.crkAmp > 1.0e-4f)
                {
                    x += crkGain * v.crkAmp * v.grainRng.bi();
                    v.crkAmp *= crkDecay;
                }
            }

            buf[i] = x;

            if (v.previewRemain > 0 && --v.previewRemain == 0) releaseVoice (v);
            if (! v.frozen && v.pos >= len - 2) { finished = true; for (int j = i + 1; j < n; ++j) { buf[j] = 0.0f; bufH[j] = 0.0f; } break; }
            if (finished) { for (int j = i + 1; j < n; ++j) { buf[j] = 0.0f; bufH[j] = 0.0f; } break; }
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
        if (split)
        {
            // upper partials with their own direction; afterwards both parts go to the room together
            float* bufHRamp = fxScratch.data() + maxBlock;
            for (int i = 0; i < n; ++i) bufHRamp[i] = bufH[i] * (float) i;
            for (int c = 0; c < usedChannels; ++c)
            {
                const float a = v.gh0[(size_t) c], b = v.gh1[(size_t) c];
                float* o = out.getWritePointer (c) + start;
                const float ga = a + (b - a) * (float) start * inv;
                const float dg = (b - a) * inv;
                if (std::abs (ga) > 1.0e-9f) juce::FloatVectorOperations::addWithMultiply (o, bufH, ga, n);
                if (std::abs (dg) > 1.0e-12f) juce::FloatVectorOperations::addWithMultiply (o, bufHRamp, dg, n);
            }
            juce::FloatVectorOperations::add (buf, bufH, n);
            juce::FloatVectorOperations::add (bufRamp, bufHRamp, n);
        }
        {
            // room sends: plain sum + circular harmonics of the pipe direction (start / end of the slice)
            const double a0 = juce::degreesToRadians ((double) v.sendAz0), a1 = juce::degreesToRadians ((double) v.sendAz1);
            const double c0 = std::cos (a0), s0 = std::sin (a0), c1 = std::cos (a1), s1 = std::sin (a1);
            double cm0 = 1.0, sm0 = 0.0, cm1 = 1.0, sm1 = 0.0;          // cos / sin (m * angle), m = 0
            for (int k = 0; k < roomSends; ++k)
            {
                float w0 = 1.0f, w1 = 1.0f;
                if (k > 0)
                {
                    if (k % 2 == 1)
                    {
                        // next m: rotate (cos, sin) by the angle
                        const double t0 = cm0 * c0 - sm0 * s0; sm0 = sm0 * c0 + cm0 * s0; cm0 = t0;
                        const double t1 = cm1 * c1 - sm1 * s1; sm1 = sm1 * c1 + cm1 * s1; cm1 = t1;
                        w0 = (float) cm0; w1 = (float) cm1;
                    }
                    else { w0 = (float) sm0; w1 = (float) sm1; }
                }
                const float g0 = v.send0 * w0, g1 = v.send1 * w1;
                const float sa = g0 + (g1 - g0) * (float) start * inv;
                const float sd = (g1 - g0) * inv;
                float* dst = roomSend[(size_t) k].data() + roomOffset + start;
                if (std::abs (sa) > 1.0e-12f) juce::FloatVectorOperations::addWithMultiply (dst, buf, sa, n);
                if (std::abs (sd) > 1.0e-12f) juce::FloatVectorOperations::addWithMultiply (dst, bufRamp, sd, n);
            }
        }
        if (v.erSlot >= 0)
        {
            float* ring = erRing.data() + (size_t) v.erSlot * (size_t) kErRing;
            const uint32_t base = erClock + (uint32_t) start;
            for (int i = 0; i < n; ++i) ring[(base + (uint32_t) i) & (kErRing - 1)] += buf[i];
        }
        if (finished) { v.active = false; releaseSlot (v.erSlot); }
    }

    // noise one-shots
    for (auto& nv : noise)
    {
        if (! nv.active) continue;
        for (int i = 0; i < n; ++i)
        {
            if (nv.pos >= nv.length - 2) { nv.active = false; releaseSlot (nv.erSlot); break; }
            nv.lp += nv.lpA * (readSample (nv.data, nv.length, nv.pos) - nv.lp);
            const float x = nv.lp * nv.gain;
            nv.pos += nv.rate;
            if (nv.erSlot >= 0) erRing[(size_t) nv.erSlot * (size_t) kErRing + ((erClock + (uint32_t) (start + i)) & (kErRing - 1))] += x;
            for (int c = 0; c < usedChannels; ++c)
                if (std::abs (nv.g[(size_t) c]) > 1.0e-9f) out.getWritePointer (c)[start + i] += x * nv.g[(size_t) c];
            for (int k = 0; k < roomSends; ++k)
                roomSend[(size_t) k][(size_t) (roomOffset + start + i)] += x * nv.send * nv.sendW[(size_t) k];
        }
    }

    // wind: omnidirectional (W) in Ambisonics, centre in stereo
    if (library != nullptr && ! library->wind.empty())
    {
        const auto& w = *library->wind.front();
        const int wl = w.size();
        const float target = p.wind * 2.0f;
        const double wr = w.sampleRate / sr;
        if (wl > 4 && (target > 0.0001f || windGain > 0.0001f) && p.fx.windRound)
        {
            // four uncorrelated parts of the recording from four directions (tetrahedron): the blower is all around
            static const float dirs[4][2] { { 45.0f, 35.0f }, { 135.0f, -35.0f }, { -135.0f, 35.0f }, { -45.0f, -35.0f } };
            std::array<std::array<float, kMaxAmbiChannels>, 4> wg {};
            for (int d = 0; d < 4; ++d) gainsFor ({ dirs[d][0], dirs[d][1], kRefDist }, wg[(size_t) d].data());
            const double quarter = (double) (wl - 2) / 4.0;
            for (int i = 0; i < n; ++i)
            {
                windGain += 0.0005f * (target - windGain);
                const float gst = turb > 0.0f ? gust[(size_t) (i >> 4)] : 0.0f;
                const float tr = p.tremOn ? 0.5f * p.tremDepth * trem[(size_t) (i >> 4)] : 0.0f;
                const float g = 0.5f * windGain * juce::jmax (0.0f, 1.0f + 1.6f * turb * gst + 1.5f * turb + tr);
                for (int d = 0; d < 4; ++d)
                {
                    double ps = windPos + quarter * d;
                    if (ps >= wl - 2) ps -= (wl - 2);
                    const float x = readSample (w.data.data(), wl, ps) * g;
                    for (int c = 0; c < usedChannels; ++c) out.getWritePointer (c)[start + i] += x * wg[(size_t) d][(size_t) c];
                }
                windPos += wr;
                if (windPos >= wl - 2) windPos -= (wl - 2);
            }
        }
        else if (wl > 4 && (target > 0.0001f || windGain > 0.0001f))
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

// ---------------------------------------------------------------------------
// simulated room ("Saal"): early reflections (image sources, per sound) and the late tail (FDN)
// ---------------------------------------------------------------------------
int OrganEngine::allocSlot (int voice, int noiseVoice) noexcept
{
    int best = -1, bestDrain = std::numeric_limits<int>::max();
    for (int i = 0; i < (int) erSlots.size(); ++i)
    {
        auto& s = erSlots[(size_t) i];
        if (! s.active) { best = i; break; }
        if (s.voice < 0 && s.noiseVoice < 0 && s.drain < bestDrain) { bestDrain = s.drain; best = i; }   // ringing out: the oldest may go
    }
    if (best < 0) return -1;                   // all busy: this sound gets no own reflections (the tail still has it)
    auto& s = erSlots[(size_t) best];
    s.active = true; s.fresh = true;
    s.voice = voice; s.noiseVoice = noiseVoice;
    s.drain = 0; s.numTaps = 0;
    for (auto& t : s.taps) t.lp = 0.0f;
    std::fill_n (erRing.data() + (size_t) best * (size_t) kErRing, kErRing, 0.0f);
    return best;
}

void OrganEngine::releaseSlot (int& slot) noexcept
{
    if (slot < 0 || slot >= (int) erSlots.size()) { slot = -1; return; }
    auto& s = erSlots[(size_t) slot];
    s.voice = -1; s.noiseVoice = -1;
    float maxD = 0.0f;
    for (int t = 0; t < s.numTaps; ++t) maxD = juce::jmax (maxD, s.taps[(size_t) t].d1);
    s.drain = (int) maxD + 512;
    slot = -1;
}

void OrganEngine::updateSlotTaps (ErSlot& s, Pos srcPos, float base, const EngineParams& p) noexcept
{
    const auto& r = p.room;
    const Vec3f L = listenerPos (r), S = roomPoint (r, srcPos);
    const float rd = seenFrom (L, S).dist;
    std::array<ImageSource, kMaxTaps> ims;
    const int n = imageSources (r, S, r.order, ims.data(), kMaxTaps);
    const auto ab = BandAbsorption::from (r);
    const float Rw = std::sqrt (1.0f - ab.wallLow), Rf = std::sqrt (1.0f - ab.floorLow);
    const float shW = std::sqrt ((1.0f - ab.wallHigh) / juce::jmax (1e-4f, 1.0f - ab.wallLow));
    const float shF = std::sqrt ((1.0f - ab.floorHigh) / juce::jmax (1e-4f, 1.0f - ab.floorLow));
    const float spec = std::sqrt (juce::jlimit (0.0f, 1.0f, 1.0f - r.scatter));     // the scattered part goes to the tail
    const float airDiff = 0.5f * (airHigh (p.air) - airLow());
    const int encOrder = order == 0 ? 0 : juce::jmin (order, 3);
    const float maxDelay = (float) (kErRing - 4 * kSubBlock);
    float tmp[kMaxAmbiChannels];
    for (int i = 0; i < n; ++i)
    {
        const auto& im = ims[(size_t) i];
        const Pos seen = seenFrom (L, im.pos);
        float delay = (seen.dist - rd) / kSpeedOfSoundRoom * (float) sr;
        float gain = base * distanceGain (seen.dist, distAmountNow) * r.erGain
                   * std::pow (Rw * spec, (float) im.hitsWalls) * std::pow (Rf * spec, (float) im.hitsFloor);
        if (delay > maxDelay) { delay = maxDelay; gain = 0.0f; }       // longer than the buffer: left to the tail
        auto& t = s.taps[(size_t) i];
        t.d1 = juce::jmax (0.0f, delay);
        t.g1 = gain;
        t.r1 = std::pow (shW, (float) im.hitsWalls) * std::pow (shF, (float) im.hitsFloor) * std::exp (-airDiff * seen.dist);
        std::fill (tmp, tmp + kMaxAmbiChannels, 0.0f);
        // first reflections sharp (up to 3rd order), the later ones at first order: they are many, quieter and
        // more diffuse anyway, and this keeps the cost down
        const int o = (im.hitsWalls + im.hitsFloor == 1) ? encOrder : juce::jmin (encOrder, 1);
        if (encOrder == 0) { encodeStereo (seen.az, tmp); t.ch = 2; }
        else { encodeAmbiX (seen.az, seen.el, o, tmp); t.ch = (o + 1) * (o + 1); }
        std::copy (tmp, tmp + kErCh, t.enc.begin());
        if (s.fresh || i >= s.numTaps) { t.d0 = t.d1; t.g0 = t.g1; t.r0 = t.r1; t.lp = 0.0f; }
    }
    for (int i = n; i < s.numTaps; ++i) s.taps[(size_t) i].g1 = 0.0f;      // order lowered: fade the extra taps out
    s.numTaps = juce::jmax (n, s.fresh ? n : s.numTaps);
    s.fresh = false;
}

void OrganEngine::renderRoom (juce::AudioBuffer<float>& out, int n, const EngineParams& p)
{
    const auto& r = p.room;
    roomW = r.W(); roomL = r.L(); roomH = r.H(); roomLx = r.listenerX; roomLy = r.listenerY;
    roomAbsW = r.absWalls; roomAbsF = r.absFloor; roomHighD = r.highDamp; roomScat = r.scatter; roomAir = p.air;
    roomOutOrder = order; roomOn = roomOnNow;
    if (! roomOnNow || n <= 0)
    {
        erClock += (uint32_t) juce::jmax (0, n);
        tailGain = 0.0f; tailWasOn = false;
        resetFdn(); convRingOut = fdnRingOut = 0;
        erActive = 0;
        tailSerialInUse = tail.oldestSerialInUse();
        return;
    }
    const int K = juce::jmin (usedChannels, order == 0 ? 2 : (juce::jmin (order, 3) + 1) * (juce::jmin (order, 3) + 1));
    const float inv = 1.0f / (float) n;
    float* y = fxScratch.data();

    // ---- early reflections
    const float shelfA = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 2500.0f / (float) sr);
    int activeCount = 0;
    for (size_t si = 0; si < erSlots.size(); ++si)
    {
        auto& s = erSlots[si];
        if (! s.active) continue;
        ++activeCount;
        if (s.voice >= 0)
        {
            const auto& v = voices[(size_t) s.voice];
            if (v.active && v.erSlot == (int) si) updateSlotTaps (s, v.erPos, v.erBase, p);
            else { s.voice = -1; s.drain = kErRing; }
        }
        else if (s.noiseVoice >= 0)
        {
            const auto& nv = noise[(size_t) s.noiseVoice];
            if (nv.active && nv.erSlot == (int) si) updateSlotTaps (s, nv.where, 1.0f, p);
            else { s.noiseVoice = -1; s.drain = kErRing; }
        }
        else if ((s.drain -= n) <= 0) { s.active = false; continue; }

        const float* ring = erRing.data() + si * (size_t) kErRing;
        for (int ti = 0; ti < s.numTaps; ++ti)
        {
            auto& t = s.taps[(size_t) ti];
            if (t.g0 < 3.0e-5f && t.g1 < 3.0e-5f) { t.d0 = t.d1; t.g0 = t.g1; t.r0 = t.r1; continue; }     // below -90 dB
            for (int i = 0; i < n; ++i)
            {
                const float f = (float) (i + 1) * inv;
                const float d = t.d0 + (t.d1 - t.d0) * f;
                // integer and fractional part of the delay kept apart: the clock itself grows without end, and
                // as a float it would lose the fraction after a few minutes (aliasing, a high ringing)
                const int di = (int) d;
                const float fd = d - (float) di;
                const uint32_t at = erClock + (uint32_t) i - (uint32_t) di;          // read position + fd samples earlier
                const float a = ring[(at - 1u) & (kErRing - 1)], b = ring[at & (kErRing - 1)];
                const float x = b + fd * (a - b);
                t.lp += shelfA * (x - t.lp);
                const float rr = t.r0 + (t.r1 - t.r0) * f;
                y[i] = (t.lp + rr * (x - t.lp)) * (t.g0 + (t.g1 - t.g0) * f);
            }
            const int kc = juce::jmin (K, t.ch);
            for (int c = 0; c < kc; ++c)
                if (std::abs (t.enc[(size_t) c]) > 1.0e-6f) juce::FloatVectorOperations::addWithMultiply (out.getWritePointer (c), y, t.enc[(size_t) c], n);
            t.d0 = t.d1; t.g0 = t.g1; t.r0 = t.r1;
        }
    }
    erActive = activeCount;

    // ---- late reverb. The decay comes from the ray tracer (the processor builds the response in the
    // background); two ways to make the tail: convolution with a synthesised response, or a feedback delay network.
    const TailIR* ir = tail.current();
    const float rtL = ir != nullptr ? ir->rtLow : eyringRt (r, false, p.air);
    const float rtH = ir != nullptr ? ir->rtHigh : eyringRt (r, true, p.air);
    const float V = roomVolume (r);
    const float rc = juce::jmax (0.2f, 0.057f * std::sqrt (V / rtL));          // critical distance
    roomRtLow = rtL; roomRtHigh = rtH; roomCritDist = rc;
    // reverberant level from the critical distance, softly capped at +4 dB so very long, hard rooms do not drown everything
    const float level = juce::jmin (1.6f, distanceGain (rc, distAmountNow)) * r.tailGain;
    const bool useFdn = r.tailModel == 1;
    if (useFdn != (lastTailModel == 1))
    {
        // switched: the other one keeps ringing out with a silent input
        if (useFdn) convRingOut = TailIR::maxLength (sr);
        else        fdnRingOut = (int) (juce::jmin (30.0f, 1.5f * juce::jmax (rtL, 0.1f)) * (float) sr);
        lastTailModel = useFdn ? 1 : 0;
    }
    const float* in = roomSend[0].data() + roomOffset;
    const int tailCh = juce::jmin (usedChannels, kMaxAmbiChannels);
    std::array<float*, kMaxAmbiChannels> o {};
    for (int c = 0; c < tailCh; ++c) o[(size_t) c] = out.getWritePointer (c);

    if (! useFdn || convRingOut > 0)
    {
        if (! tailWasOn) { tail.reset(); tailWasOn = true; }
        const float g0 = tailGain, g1 = useFdn ? g0 : g0 + (level - g0) * (1.0f - std::exp (-(float) n / (0.05f * (float) sr)));
        tailGain = g1;
        // the response has unit energy (W)
        tail.process (useFdn ? zeroIn.data() : in, o.data(), tailCh, n, g0, g1);
        if (useFdn && (convRingOut -= n) <= 0) { convRingOut = 0; tailWasOn = false; tailGain = 0.0f; }
    }
    if (useFdn && convRingOut == 0) tail.adoptLatest();          // keeps the ray-traced decay current for the FDN
    if (useFdn || fdnRingOut > 0)
    {
        renderFdn (o.data(), K, useFdn ? in : zeroIn.data(), n, rtL, rtH, useFdn ? level : fdnGain, r);
        if (! useFdn && (fdnRingOut -= n) <= 0) { fdnRingOut = 0; resetFdn(); }
    }
    tailSerialInUse = tail.oldestSerialInUse();
    erClock += (uint32_t) n;
}

void OrganEngine::resetFdn() noexcept
{
    fdnGain = 0.0f; fdnPreDelay = -1.0f; fdnEncOrder = -1;          // the next use clears the lines
}

void OrganEngine::renderFdn (float* const* o, int K, const float* in, int n, float rtL, float rtH, float gainLevel, const RoomParams& r)
{
    // 16 delay lines (lengths around the mean free path), a random orthogonal mixing matrix, two-band decay
    // per line (ray-traced reverberation times), slow delay modulation, lines spread over 16 directions
    const float mfp = meanFreePath (r);
    rtL = juce::jlimit (0.05f, 30.0f, rtL);
    rtH = juce::jlimit (0.05f, 30.0f, juce::jmin (rtH, rtL));
    const float inv = 1.0f / (float) n;
    static const float ratios[kFdnLines] { 0.53f, 0.59f, 0.67f, 0.71f, 0.77f, 0.83f, 0.89f, 0.97f, 1.03f, 1.09f, 1.13f, 1.21f, 1.27f, 1.33f, 1.41f, 1.49f };
    const float k = 1.0f - std::exp (-(float) n / (0.08f * (float) sr));
    std::array<float, kFdnLines> dOld = fdnDelay, dNew {};
    const bool first = fdnPreDelay < 0.0f;
    if (first) { std::fill (fdnBuf.begin(), fdnBuf.end(), 0.0f); std::fill (fdnPre.begin(), fdnPre.end(), 0.0f); fdnLp.fill (0.0f); }
    float gMean = 0.0f;
    for (int j = 0; j < kFdnLines; ++j)
    {
        const float target = juce::jlimit (64.0f, (float) (kFdnLen - 4 * kSubBlock), mfp / kSpeedOfSoundRoom * ratios[j] * (float) sr);
        if (first) dOld[(size_t) j] = target;
        dNew[(size_t) j] = dOld[(size_t) j] + (target - dOld[(size_t) j]) * k;
        fdnGL[(size_t) j] = std::pow (10.0f, -3.0f * dNew[(size_t) j] / ((float) sr * rtL));
        fdnGH[(size_t) j] = std::pow (10.0f, -3.0f * dNew[(size_t) j] / ((float) sr * rtH));
        gMean += fdnGL[(size_t) j] / (float) kFdnLines;
    }
    fdnDelay = dNew;
    const float preTarget = juce::jlimit (0.0f, (float) (kFdnLen - 4 * kSubBlock), 0.5f * mfp / kSpeedOfSoundRoom * (float) sr);
    const float pre0 = first ? preTarget : fdnPreDelay;
    const float pre1 = pre0 + (preTarget - pre0) * k;
    fdnPreDelay = pre1;
    const float gainTarget = std::sqrt (juce::jmax (1.0e-6f, 1.0f - gMean * gMean)) * kFdnLevel * gainLevel;
    const float gain0 = first ? 0.0f : fdnGain, gain1 = gain0 + (gainTarget - gain0) * k;
    fdnGain = gain1;
    const int encOrder = order == 0 ? 0 : juce::jmin (order, 3);
    if (encOrder != fdnEncOrder)
    {
        fdnEncOrder = encOrder;
        for (int j = 0; j < kFdnLines; ++j)
        {
            const float zz = 1.0f - 2.0f * ((float) j + 0.5f) / (float) kFdnLines;
            const float az = std::fmod (137.508f * (float) j, 360.0f) - 180.0f;
            const float el = juce::radiansToDegrees (std::asin (zz));
            auto& e = fdnEnc[(size_t) j];
            std::fill (e.begin(), e.end(), 0.0f);
            if (encOrder == 0) encodeStereo (az, e.data());
            else encodeAmbiX (az, el, encOrder, e.data());
        }
    }
    const float splitA = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 1500.0f / (float) sr);
    constexpr uint32_t mask = kFdnLen - 1;
    std::array<float, kFdnLines> yl {};
    const float modDepth = (float) (8.0 * sr / 48000.0);
    // read position: the integer clock and the fractional delay are kept apart (a float clock would lose
    // the fraction after a few minutes - aliasing, a high ringing)
    auto readLine = [mask] (const float* line, uint32_t now, float d)
    {
        const int di = (int) d;
        const float fd = d - (float) di;
        const uint32_t at = now - (uint32_t) di;
        const float b = line[at & mask], a = line[(at - 1u) & mask];
        return b + fd * (a - b);
    };
    for (int i = 0; i < n; ++i)
    {
        const float f = (float) (i + 1) * inv;
        fdnPre[fdnPos & mask] = in[i];
        const float x = readLine (fdnPre.data(), fdnPos, pre0 + (pre1 - pre0) * f);
        for (int j = 0; j < kFdnLines; ++j)
        {
            // slow modulation of the delay (+-8 samples at 48 kHz): no fixed modes that ring
            const float c = fdnModC[(size_t) j], sn = fdnModS[(size_t) j];
            fdnModC[(size_t) j] = c * fdnModStepC[(size_t) j] - sn * fdnModStepS[(size_t) j];
            fdnModS[(size_t) j] = sn * fdnModStepC[(size_t) j] + c * fdnModStepS[(size_t) j];
            const float d = dOld[(size_t) j] + (dNew[(size_t) j] - dOld[(size_t) j]) * f + modDepth * sn;
            const float v = readLine (fdnBuf.data() + (size_t) j * (size_t) kFdnLen, fdnPos, d);
            fdnLp[(size_t) j] += splitA * (v - fdnLp[(size_t) j]);
            yl[(size_t) j] = fdnGL[(size_t) j] * fdnLp[(size_t) j] + fdnGH[(size_t) j] * (v - fdnLp[(size_t) j]);
        }
        for (int j = 0; j < kFdnLines; ++j)
        {
            const auto& row = fdnMatrix[(size_t) j];
            float h = 0.0f;
            for (int k2 = 0; k2 < kFdnLines; ++k2) h += row[(size_t) k2] * yl[(size_t) k2];
            fdnBuf[(size_t) j * (size_t) kFdnLen + (fdnPos & mask)] = h + ((j & 1) ? -0.25f : 0.25f) * x;
        }
        const float g = gain0 + (gain1 - gain0) * f;
        for (int c = 0; c < K; ++c)
        {
            float s = 0.0f;
            for (int j = 0; j < kFdnLines; ++j) s += yl[(size_t) j] * fdnEnc[(size_t) j][(size_t) c];
            o[c][i] += s * g;
        }
        ++fdnPos;
    }
    for (int j = 0; j < kFdnLines; ++j)       // keep the modulation phasors on the unit circle
    {
        const float len = std::sqrt (fdnModC[(size_t) j] * fdnModC[(size_t) j] + fdnModS[(size_t) j] * fdnModS[(size_t) j]);
        if (len > 0.0f) { fdnModC[(size_t) j] /= len; fdnModS[(size_t) j] /= len; }
    }
}

// ---------------------------------------------------------------------------
// wind leaks and wandering echoes
// ---------------------------------------------------------------------------
void OrganEngine::renderLeaks (juce::AudioBuffer<float>& out, int n, const EngineParams& p)
{
    const auto& fx = p.fx;
    const int count = juce::jlimit (1, (int) leaks.size(), fx.leakCount);
    if (fx.leakLevel <= 0.0001f)
    {
        for (auto& L : leaks) { L.amp = 0.0f; L.s1 = L.s2 = 0.0f; L.g1.fill (0.0f); }
        return;
    }
    const float inv = 1.0f / (float) n;
    float* buf = fxScratch.data();
    float* ramp = fxScratch.data() + maxBlock;
    const float ampK = 1.0f - std::exp (-1.0f / (0.02f * (float) sr));      // 20 ms smoothing
    for (int li = 0; li < (int) leaks.size(); ++li)
    {
        auto& L = leaks[(size_t) li];
        if (li >= count) { L.amp = 0.0f; L.g1.fill (0.0f); continue; }
        L.pos.az = wrapAz (L.pos.az + L.dir * fx.leakWander * chunkSeconds * (0.6f + 0.4f * L.freqMul));
        L.g0 = L.g1;
        gainsFor (L.pos, L.g1.data());
        const float dg = distanceGain (L.pos.dist, distAmountNow);
        for (auto& x : L.g1) x *= dg;

        const float fc = juce::jmin (0.45f * (float) sr, fx.leakFreq * L.freqMul);
        const float g = std::tan (juce::MathConstants<float>::pi * fc / (float) sr);
        const float k = 1.0f / 3.0f;                                          // Q 3
        const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const float lvl = 0.9f * fx.leakLevel;
        for (int i = 0; i < n; ++i)
        {
            if (--L.ampCount <= 0)
            {
                // a leak hisses unevenly: it swells, flutters, sometimes stops for a moment
                const float u = L.rng.uni();
                L.ampTarget = u < 0.12f ? 0.0f : 0.25f + 0.75f * L.rng.uni();
                L.ampCount = (int) ((0.04f + 0.7f * L.rng.uni() * L.rng.uni()) * (float) sr);
            }
            L.amp += ampK * (L.ampTarget - L.amp);
            const float x = L.rng.bi();
            const float v3 = x - L.s2;
            const float v1 = a1 * L.s1 + a2 * v3;
            const float v2 = L.s2 + a2 * L.s1 + a3 * v3;
            L.s1 = 2.0f * v1 - L.s1;
            L.s2 = 2.0f * v2 - L.s2;
            buf[i] = v1 * k * L.amp * lvl;
            ramp[i] = buf[i] * (float) i;
        }
        for (int c = 0; c < usedChannels; ++c)
        {
            const float a = L.g0[(size_t) c], b = L.g1[(size_t) c];
            float* o = out.getWritePointer (c);
            if (std::abs (a) > 1.0e-9f) juce::FloatVectorOperations::addWithMultiply (o, buf, a, n);
            if (std::abs (b - a) > 1.0e-12f) juce::FloatVectorOperations::addWithMultiply (o, ramp, (b - a) * inv, n);
        }
        // a little of it into the room, from its own direction
        const double az = juce::degreesToRadians ((double) (roomFollowNow ? L.pos.az : roomBaseAzNow));
        for (int kk = 0; kk < roomSends; ++kk)
        {
            const int m = (kk + 1) / 2;
            const float w = kk == 0 ? 1.0f : (float) (kk % 2 == 1 ? std::cos (m * az) : std::sin (m * az));
            juce::FloatVectorOperations::addWithMultiply (roomSend[(size_t) kk].data() + roomOffset, buf, 0.3f * w, n);
        }
    }
}

void OrganEngine::renderEchoes (juce::AudioBuffer<float>& out, int n, const EngineParams& p)
{
    const auto& fx = p.fx;
    const int len = (int) echoBuf.size();
    if (len < 4 * n || n <= 0) return;
    // input: the whole dry organ (W, or the middle of the stereo pair)
    const float* in0 = out.getReadPointer (0);
    const float* in1 = usedChannels > 1 ? out.getReadPointer (1) : in0;
    for (int i = 0; i < n; ++i)
    {
        const float x = order == 0 ? 0.5f * (in0[i] + in1[i]) : in0[i];
        echoBuf[(size_t) ((echoWrite + i) % len)] = x;
    }
    if (fx.echoLevel > 0.0001f)
    {
        const int repeats = juce::jlimit (1, 8, fx.echoRepeats);
        double target = (fx.echoSync ? lfoDivQuarters (fx.echoDiv) * 60.0 / juce::jmax (20.0, p.bpm) : (double) fx.echoTime) * sr;
        target = juce::jlimit ((double) n + 4.0, (double) (len - n - 8) / repeats, target);
        if (echoDelay < 0.0) echoDelay = target;
        const double d0 = echoDelay;
        const double d1 = d0 + (target - d0) * (1.0 - std::exp (-(double) chunkSeconds / 0.12));
        echoDelay = d1;
        float* y = fxScratch.data();
        std::array<float, kMaxAmbiChannels> tg {};
        for (int r = 1; r <= repeats; ++r)
        {
            const float gain = fx.echoLevel * std::pow (fx.echoFeedback, (float) (r - 1));
            if (gain < 1.0e-4f) break;
            const Pos dir { wrapAz (fx.echoTurn * (float) r), juce::jlimit (-80.0f, 80.0f, fx.echoTilt * (float) r), kRefDist };
            gainsFor (dir, tg.data());
            const float fc = juce::jmax (250.0f, 18000.0f * std::pow (1.0f - 0.85f * fx.echoDamp, (float) r));
            const float a = fc >= 17000.0f ? 1.0f : 1.0f - std::exp (-juce::MathConstants<float>::twoPi * fc / (float) sr);
            float& lp = echoLp[(size_t) (r - 1)];
            for (int i = 0; i < n; ++i)
            {
                const double dd = (d0 + (d1 - d0) * (double) i / (double) n) * r;
                double rp = (double) (echoWrite + i) - dd;
                while (rp < 0.0) rp += len;
                const int i0 = (int) rp;
                const float f = (float) (rp - i0);
                const float x0 = echoBuf[(size_t) (i0 % len)], x1 = echoBuf[(size_t) ((i0 + 1) % len)];
                lp += a * (x0 + f * (x1 - x0) - lp);
                y[i] = lp * gain;
            }
            for (int c = 0; c < usedChannels; ++c)
                if (std::abs (tg[(size_t) c]) > 1.0e-9f) juce::FloatVectorOperations::addWithMultiply (out.getWritePointer (c), y, tg[(size_t) c], n);
            const double az = juce::degreesToRadians ((double) (roomFollowNow ? dir.az : roomBaseAzNow));
            for (int kk = 0; kk < roomSends; ++kk)
            {
                const int m = (kk + 1) / 2;
                const float w = kk == 0 ? 1.0f : (float) (kk % 2 == 1 ? std::cos (m * az) : std::sin (m * az));
                juce::FloatVectorOperations::addWithMultiply (roomSend[(size_t) kk].data() + roomOffset, y, 0.35f * w, n);
            }
        }
    }
    else echoDelay = -1.0;
    echoWrite = (echoWrite + n) % len;
}

// ---------------------------------------------------------------------------
// modulation
// ---------------------------------------------------------------------------
void OrganEngine::advanceModulation (const EngineParams& p, double dt) noexcept
{
    const ModParams& m = haveLastMod ? lastMod : p.mod;       // last slice's (possibly modulated) settings
    for (int i = 0; i < kNumLfos; ++i)
    {
        const auto& L = m.lfo[(size_t) i];
        const double q = lfoDivQuarters (L.div);
        double ph = lfoPhase[(size_t) i];
        if (L.sync && p.playing) ph = p.ppq / q;
        else                     ph += (L.sync ? p.bpm / 60.0 / q : (double) L.rate) * dt;
        if (std::abs (ph) > 1.0e6) ph = std::fmod (ph, 1000.0);
        lfoPhase[(size_t) i] = ph;
        lfoPhaseOut[(size_t) i] = ph;
        globalSrc[(size_t) (srcLfo1 + i)] = lfoValue (L.shape, ph, 0);
    }
    bool any = false;
    for (int k = 0; k < kNumKeys; ++k) any = any || keyHeld[(size_t) k] || keySustained[(size_t) k];
    for (int j = 0; j < kNumEnvs; ++j)
    {
        auto& e = globalEnv[(size_t) j];
        if (globalTrig) e.trigger();
        if (! any && e.gate) e.releaseGate();
        e.advance (m.env[(size_t) j], (float) dt);
        globalSrc[(size_t) (srcEnv1 + j)] = e.level;
    }
    globalTrig = false;
    globalSrc[srcKey] = lastKeyPos;
    globalSrc[srcModWheel] = modWheel;
    globalSrc[srcRandom] = lastRandom;
    globalSrc[srcVelocity] = lastVelocity;
    globalSrc[srcPitchWheel] = pitchWheel;
    for (int i = 0; i < kNumSources; ++i) sourceOut[(size_t) i] = globalSrc[(size_t) i];
}

static inline float lfoSpreadOffset (const LfoParams& L, int stop, int key) noexcept
{
    if (L.spread <= 0.0f) return 0.0f;
    return L.spread * (L.spreadMode == 0 ? (float) key / (float) kNumKeys : hash01 ((uint32_t) (stop * 131 + key * 7 + 1)));
}

Pos OrganEngine::displayPosition (int stop, int key, const EngineParams& p) const noexcept
{
    Pos pos = currentPosition (stop, key, p);
    float az = 0.0f, el = 0.0f, dm = 1.0f;
    if ((soundingMask[(size_t) stop].load() >> key) & 1)
    {
        az = modAz[(size_t) stop][(size_t) key].load();
        el = modEl[(size_t) stop][(size_t) key].load();
        dm = modDistMul[(size_t) stop][(size_t) key].load();
    }
    else
    {
        std::array<float, kNumPipeTargets> pm {};
        bool any = false;
        for (const auto& r : p.mod.routes)
        {
            if (r.src < 0 || r.target < 0 || r.target > tgtDist || r.amount == 0.0f) continue;
            float v = 0.0f;
            if (r.src >= srcLfo1 && r.src < srcLfo1 + kNumLfos)
            {
                const auto& L = p.mod.lfo[(size_t) (r.src - srcLfo1)];
                v = lfoValue (L.shape, lfoPhaseOut[(size_t) (r.src - srcLfo1)].load() + lfoSpreadOffset (L, stop, key), 0);
            }
            else if (r.src >= srcEnv1 && r.src < srcEnv1 + kNumEnvs)
                v = p.mod.env[(size_t) (r.src - srcEnv1)].perPipe ? 0.0f : sourceOut[(size_t) r.src].load();
            else if (r.src == srcKey)      v = (float) key / (float) (kNumKeys - 1) * 2.0f - 1.0f;
            else if (r.src == srcModWheel || r.src == srcPitchWheel || r.src == srcVelocity) v = sourceOut[(size_t) r.src].load();
            pm[(size_t) r.target] += r.amount * shapeSource (r.src, v, r.uni);
            any = true;
        }
        if (! any) return pos;
        az = 180.0f * pm[tgtAz];
        el = 90.0f * pm[tgtEl];
        dm = std::exp2 (3.0f * juce::jlimit (-2.0f, 2.0f, pm[tgtDist]));
    }
    pos.az = wrapAz (pos.az + az);
    pos.el = juce::jlimit (-90.0f, 90.0f, pos.el + el);
    pos.dist = juce::jlimit (kMinDist, kMaxDist, pos.dist * dm);
    return pos;
}

float OrganEngine::voiceSource (const Voice& v, int src, const EngineParams& p) const noexcept
{
    if (src >= srcLfo1 && src < srcLfo1 + kNumLfos)
    {
        const int i = src - srcLfo1;
        const auto& L = p.mod.lfo[(size_t) i];
        return lfoValue (L.shape, lfoPhase[(size_t) i] + lfoSpreadOffset (L, v.stop, v.key), 0);
    }
    if (src >= srcEnv1 && src < srcEnv1 + kNumEnvs)
    {
        const int j = src - srcEnv1;
        return p.mod.env[(size_t) j].perPipe ? v.menv[(size_t) j].level : globalEnv[(size_t) j].level;
    }
    switch (src)
    {
        case srcKey:      return v.keyPos;
        case srcModWheel: return modWheel;
        case srcRandom:   return v.randMod;
        case srcVelocity: return v.velMod;
        case srcPitchWheel: return pitchWheel;
        default:          return 0.0f;
    }
}

void OrganEngine::process (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi, const EngineParams& pIn)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = out.getNumSamples();
    if (n <= 0) return;
    for (auto& r : roomSend) if ((int) r.size() < n) r.assign ((size_t) n, 0.0f);     // safety (prepare() sizes them)

    // render in short slices: every slice gets fresh positions, so even 10 Hz motion stays smooth
    for (int start = 0; start < n; start += kSubBlock)
    {
        const int len = juce::jmin (kSubBlock, n - start);
        juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), out.getNumChannels(), start, len);
        chunkMidi.clear();
        chunkMidi.addEvents (midi, start, len, -start);
        EngineParams pc = pIn;
        pc.ppq = pIn.ppq + (start / sr) * pIn.bpm / 60.0;
        advanceModulation (pc, len / sr);
        if (modHook != nullptr) modHook->modulate (pc, globalSrc.data());
        lastMod = pc.mod; haveLastMod = true;
        roomOffset = start;
        processChunk (view, chunkMidi, pc);
    }
    roomOffset = 0;

    // meters
    const float pk0 = out.getNumChannels() > 0 ? out.getMagnitude (0, 0, n) : 0.0f;
    const float pk1 = out.getNumChannels() > 1 ? (order == 0 ? out.getMagnitude (1, 0, n) : pk0) : 0.0f;
    meterL = juce::jmax (pk0, meterL.load() * 0.9f);
    meterR = juce::jmax (pk1, meterR.load() * 0.9f);
}

void OrganEngine::processChunk (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi, const EngineParams& pIn)
{
    EngineParams p = pIn;                // local copy with derived values
    p.deg.prepareBlock (sr);
    out.clear();
    const int numSamples = out.getNumSamples();
    if (numSamples <= 0) return;
    if ((int) scratch.size() < numSamples)       // safety (should not happen: prepare() sizes them)
    {
        scratch.assign ((size_t) numSamples, 0.0f); scratchRamp.assign ((size_t) numSamples, 0.0f);
    }
    roomSends = 2 * juce::jlimit (0, kMaxOrder, p.roomHarmonics) + 1;
    for (int k = 0; k < roomSends; ++k)
        std::fill (roomSend[(size_t) k].begin() + roomOffset, roomSend[(size_t) k].begin() + roomOffset + numSamples, 0.0f);
    {
        // clicks and stop pulls come from the console (or the common direction when not following)
        const double a = juce::degreesToRadians ((double) (p.roomFollow ? p.consoleAz : p.roomBaseAz));
        noiseRoomW[0] = 1.0f;
        for (int m = 1; 2 * m < roomSends; ++m)
        {
            noiseRoomW[(size_t) (2 * m - 1)] = (float) std::cos (m * a);
            noiseRoomW[(size_t) (2 * m)] = (float) std::sin (m * a);
        }
    }
    const double blockSeconds = numSamples / sr;

    order = juce::jlimit (0, kMaxOrder, p.outputMode);
    usedChannels = juce::jmin (out.getNumChannels(), order == 0 ? 2 : (order + 1) * (order + 1));
    distAmountNow = p.distAmount;
    consoleAzNow = p.consoleAz; consoleElNow = p.consoleEl;
    if (roomOnNow && ! p.room.on)
    {
        // room switched off: reflections stop
        for (auto& v : voices) v.erSlot = -1;
        for (auto& nv : noise) nv.erSlot = -1;
        for (auto& s : erSlots) s.active = false;
    }
    roomOnNow = p.room.on && ! erSlots.empty();
    if (roomOnNow)
        for (size_t si = 0; si < erSlots.size(); ++si)
            if (erSlots[si].active)
            {
                // this chunk's part of the ring is rewritten by the owner (or stays silent while ringing out)
                float* ring = erRing.data() + si * (size_t) kErRing;
                for (int i = 0; i < numSamples; ++i) ring[(erClock + (uint32_t) i) & (kErRing - 1)] = 0.0f;
            }
    keySource = p.tuning.source;
    {
        // free tuning on / off: let go of everything first (notes are counted differently)
        const bool fm = p.notes != nullptr;
        if (fm != freeMode) { releaseAllKeys (p); freeMode = fm; }
        notesNow = p.notes;
        notesContinuous = p.notesContinuous;
    }
    roomFollowNow = p.roomFollow;
    roomBaseAzNow = p.roomBaseAz;
    chunkSeconds = (float) blockSeconds;

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
            if (p.stopOn[(size_t) s] && keyHeld[(size_t) k] && ! freeMode) startVoice (s, k);
            if (! p.stopOn[(size_t) s]) releaseStopKey (s, k);
        }
        if (p.stopOn[(size_t) s] && freeMode)
            for (int id = 0; id < NoteTable::kSize; ++id)
                if (freeNotes[(size_t) id].held) startVoice (s, freeNotes[(size_t) id].key, -1, id);
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

    // ---- effects: due events (delayed sounds, pipes waiting for the tracker path), moving sounds
    {
        int w = 0;
        for (int i = 0; i < numScheduled; ++i)
        {
            auto e = scheduled[(size_t) i];
            if (e.delay >= numSamples) { e.delay -= numSamples; scheduled[(size_t) w++] = e; continue; }
            if (! e.voice)
            {
                if (e.sample != nullptr && library != nullptr) spawnSound (*e.sample, e.gain, e.tone, e.rate, e.pos, e.azVel, e.send);
            }
            else if (library != nullptr && p.stopOn[(size_t) e.stop])
            {
                startVoice (e.stop, e.key, -1, e.noteId);
                // key already let go: the pallet still opened, but closes right away
                const bool stillDown = e.noteId >= 0 ? freeNotes[(size_t) e.noteId].held
                                                     : (keyHeld[(size_t) e.key] || keySustained[(size_t) e.key]);
                if (! stillDown)
                    for (auto& v : voices)
                        if (v.active && v.held && v.stop == e.stop && v.key == e.key && v.previewRemain < 0 && v.noteId == e.noteId) releaseVoice (v);
            }
        }
        numScheduled = w;
    }
    for (auto& nv : noise)
        if (nv.active && nv.azVel != 0.0f)
        {
            nv.where.az = wrapAz (nv.where.az + nv.azVel * (float) blockSeconds);
            placedGains (nv);
        }
    partRot = std::fmod (partRot + (double) p.fx.partTurn * 360.0 * blockSeconds, 360.0);

    // ---- spatial data for this block (end-of-block targets, ramped from the previous ones)
    for (auto& v : voices)
    {
        if (! v.active) continue;
        if (v.noteId >= 0 && notesContinuous && v.held && notesNow != nullptr && notesNow->hz[(size_t) v.noteId] > 1.0f)
            v.noteCents = centsFor (v.noteId, v.key);          // MTS-ESP: held notes follow a changing tuning
        if (! v.fresh) { v.g0 = v.g1; v.gh0 = v.gh1; v.send0 = v.send1; v.sendAz0 = v.sendAz1; v.modCents0 = v.modCents1; }
        updateVoiceSpatial (v, p, blockSeconds);
        if (v.fresh) { v.g0 = v.g1; v.gh0 = v.gh1; v.send0 = v.send1; v.sendAz0 = v.sendAz1; v.modCents0 = v.modCents1; v.fresh = false; }
    }
    auto initFresh = [&] ()
    {
        for (auto& v : voices)
            if (v.active && v.fresh)
            {
                updateVoiceSpatial (v, p, blockSeconds);
                v.g0 = v.g1; v.gh0 = v.gh1; v.send0 = v.send1; v.sendAz0 = v.sendAz1; v.modCents0 = v.modCents1; v.fresh = false;
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
    renderLeaks (out, numSamples, p);

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

    renderEchoes (out, numSamples, p);
    renderRoom (out, numSamples, p);

    // ---- master gain (smoothed) + meters
    const float step = (p.masterGain - masterSmoothed) / (float) numSamples;
    for (int c = 0; c < usedChannels; ++c)
    {
        float g = masterSmoothed;
        float* o = out.getWritePointer (c);
        for (int i = 0; i < numSamples; ++i) { g += step; o[i] *= g; }
    }
    {
        for (int k = 0; k < roomSends; ++k)
        {
            float gk = masterSmoothed;
            for (int i = 0; i < numSamples; ++i) { gk += step; roomSend[(size_t) k][(size_t) (roomOffset + i)] *= gk; }
        }
    }
    masterSmoothed = p.masterGain;

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
