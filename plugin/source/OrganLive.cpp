// "Lebendige Orgel": the parts of OrganEngine that treat the organ as one physical thing -
// a shared wind supply, a walking listener, wear, sympathetic pipes, adaptive pure tuning, the Tonnetz walker
#include "OrganEngine.h"

namespace rysum
{
// ---------------------------------------------------------------------------
// the listener walks
// ---------------------------------------------------------------------------
Pos OrganEngine::heardFrom (Pos pos) const noexcept
{
    if (! liveWalk) return pos;
    const float a = juce::degreesToRadians (pos.az), e = juce::degreesToRadians (pos.el);
    const float x = -std::sin (a) * std::cos (e) * pos.dist - walkX, y = std::cos (a) * std::cos (e) * pos.dist - walkY;
    const float z = std::sin (e) * pos.dist;
    const float rh = std::sqrt (x * x + y * y), r = std::sqrt (rh * rh + z * z);
    Pos q;
    // (walking right through a pipe: never nearer than a metre, or than it was)
    q.dist = juce::jlimit (juce::jmin (1.0f, pos.dist), kMaxDist, r);
    q.az = rh > 1.0e-5f ? juce::radiansToDegrees (std::atan2 (-x, y)) : pos.az;
    q.el = r > 1.0e-5f ? juce::radiansToDegrees (std::asin (juce::jlimit (-1.0f, 1.0f, z / r))) : pos.el;
    return q;
}

/** the pitch a pipe has in the current tuning, whether it sounds or not (0 = no such pipe) */
float OrganEngine::pipeHzIdeal (int stop, int key, const EngineParams& p) const noexcept
{
    if (library == nullptr) return 0.0f;
    const Pipe& pp = library->pipes[(size_t) stop][(size_t) juce::jlimit (0, kNumKeys - 1, keySource[(size_t) key])];
    if (! pp.valid) return 0.0f;
    double ratio = 1.0;
    if (pp.substituted && pp.substituteFrom >= 0)
        ratio = meantoneHz (pp.midi) / meantoneHz (kPhysKeys[(size_t) juce::jlimit (0, kNumKeys - 1, pp.substituteFrom)]);
    const double f = (p.beatIdeal ? (pp.nominalHz > 0.0 ? pp.nominalHz : pp.measuredHz) : (pp.measuredHz > 0.0 ? pp.measuredHz : pp.nominalHz)) * ratio;
    return (float) (f * std::exp2 ((p.tuning.cents[(size_t) key] + p.pitchCents) / 1200.0));
}

// ---------------------------------------------------------------------------
// once per slice: bellows, listener, wear; pure tuning and the choice of sympathetic pipes
// ---------------------------------------------------------------------------
void OrganEngine::updateLive (const EngineParams& p, double dt) noexcept
{
    const auto& lv = p.live;
    const float fdt = (float) dt;

    // ---- the listener: a place (which can be modulated like any slider) and a walk around it
    {
        float tx = lv.listenX, ty = lv.listenY;
        if (lv.walkShape > 0 && lv.walkSize > 0.01f)
        {
            walkPhase += (double) lv.walkRate * dt;
            walkPhase -= std::floor (walkPhase);
            const float a = juce::MathConstants<float>::twoPi * (float) walkPhase, s = lv.walkSize;
            switch (lv.walkShape)
            {
                case 1:  tx += s * std::sin (a); ty += s * std::cos (a); break;                // a circle
                case 2:  tx += s * std::sin (a); break;                                        // left and right
                case 3:  ty += s * std::sin (a); break;                                        // forward and back
                default: tx += s * std::sin (a); ty += 0.5f * s * std::sin (2.0f * a); break;  // a figure eight
            }
        }
        // the 3D view: the camera walks, the listener with it
        const bool walking3d = viewWalkOn.load();
        if (walking3d) { tx += viewX.load(); ty += viewY.load(); }
        // (a switched shape or a typed value: the listener walks there quickly instead of jumping)
        const float k = 1.0f - std::exp (-fdt / (walking3d ? 0.04f : 0.12f));
        walkX += k * (tx - walkX); walkY += k * (ty - walkY);
        if (std::abs (tx) + std::abs (ty) < 1.0e-4f && std::abs (walkX) + std::abs (walkY) < 0.003f) walkX = walkY = 0.0f;
        liveWalk = walkX != 0.0f || walkY != 0.0f;
        listenerXNow = walkX; listenerYNow = walkY;
    }

    // ---- Balg: every sounding pipe takes wind from the same supply (low pipes much more than high ones)
    if (lv.bellows <= 0.0001f)
    {
        bellowsP += (1.0f - std::exp (-fdt / 0.05f)) * (1.0f - bellowsP);
        bellowsV = 1.0f; bellowsRising = false;
    }
    else
    {
        float demand = 0.0f;
        for (const auto& v : voices)
            if (v.active && v.held && ! v.killing)
            {
                const float hz = v.hzNow > 8.0f ? v.hzNow : (float) juce::jmax (30.0, v.pipe->nominalHz);
                demand += p.stopWind[(size_t) v.stop] * v.palWind * juce::jlimit (0.35f, 3.0f, std::sqrt (261.6f / hz));
            }
        if (lv.blower == 0)
        {
            // a blower: it feeds what is missing, a big chord pulls the pressure down for as long as it sounds
            const float tau = juce::jmax (0.03f, 0.1f * lv.bellowsSize);
            const float pull = 3.0f * lv.bellows * lv.bellows * demand / 24.0f;
            bellowsP += ((1.0f - bellowsP) - pull * bellowsP) * juce::jmin (1.0f, fdt / tau);
            bellowsV = bellowsP; bellowsRising = false;
        }
        else
        {
            // a Kalkant: the bellows is lifted with every tread and then sinks as the pipes use the wind
            bool tread = false;
            if (lv.blower == 2)
            {
                static const double quarters[] { 32.0, 16.0, 8.0, 4.0, 2.0, 1.0 };
                const double q = quarters[juce::jlimit (0, 5, lv.treadDiv)];
                if (p.playing)
                {
                    const double idx = std::floor (p.ppq / q);
                    if (idx != treadLast) { tread = true; treadLast = idx; }
                }
                else if ((treadPhase += p.bpm / 60.0 / q * dt) >= 1.0) { treadPhase -= std::floor (treadPhase); tread = true; }
            }
            else if ((treadPhase += (double) lv.treadRate / 60.0 * dt) >= 1.0) { treadPhase -= std::floor (treadPhase); tread = true; }
            if (tread) { bellowsRising = true; treadEnv = 1.0f; treadCount = treadCount.load() + 1; }
            if (bellowsRising && (bellowsV += fdt / 0.3f) >= 1.0f) { bellowsV = 1.0f; bellowsRising = false; }
            // (twelve middle pipes empty it in 'size' seconds at full amount)
            bellowsV = juce::jmax (0.0f, bellowsV - lv.bellows * (demand / 12.0f) / juce::jmax (0.2f, lv.bellowsSize) * fdt);
            // the weight on the bellows keeps the pressure up until it is nearly down
            const float t = juce::jlimit (0.0f, 1.0f, bellowsV / 0.3f), target = t * t * (3.0f - 2.0f * t);
            bellowsP += (1.0f - std::exp (-fdt / 0.04f)) * (target - bellowsP);
        }
    }
    bellowsNow = bellowsP; bellowsFillNow = bellowsV;

    // ---- Verschleiss: every pipe remembers how long it has sounded
    if (wearReset.exchange (false))
    {
        for (auto& st : wear) for (auto& w : st) w = 0.0f;
        for (auto& v : voices) v.wearNow = 0.0f;
    }
    if (lv.wear > 0.0001f)
    {
        const float d = fdt / (60.0f * juce::jmax (0.05f, lv.wearMinutes));
        for (auto& v : voices)
            if (v.active && v.held && v.previewRemain < 0)
            {
                auto& w = wear[(size_t) v.stop][(size_t) v.key];
                v.wearNow = juce::jmin (1.0f, w.load() + d);
                w = v.wearNow;
            }
    }

    updatePure (p, dt);

    // ---- sympathetic pipes: chosen anew about 20 times a second
    const bool sympOn = lv.sympLevel > 1.0e-5f;
    if ((sympOn || sympNow.load() > 0) && --sympTick <= 0) { sympTick = 32; updateSympathy (p); }
    sympBusOn = sympOn || sympNow.load() > 0;
    if (sympBusOn) std::fill (sympBus.begin(), sympBus.begin() + juce::jmin ((int) sympBus.size(), kSubBlock), 0.0f);
}

// ---------------------------------------------------------------------------
// Reinheit: the held notes pull each other towards pure intervals (springs between every pair, a weak one to the
// temperament so the chord stays where it is)
// ---------------------------------------------------------------------------
void OrganEngine::updatePure (const EngineParams& p, double dt) noexcept
{
    const float amt = p.live.pure;
    if (amt <= 0.0001f)
    {
        if (pureHash != 0 || pureNow.load() != 0.0f)
        {
            const float k = std::exp (-(float) dt / 0.05f);
            float mx = 0.0f;
            for (auto& v : voices)
            {
                v.pureT = 0.0f;
                v.pureC = std::abs (v.pureC) < 0.02f ? 0.0f : v.pureC * k;
                if (v.active) mx = juce::jmax (mx, std::abs (v.pureC));
            }
            pureHash = 0; pureNow = mx;
        }
        return;
    }
    struct Note { int key, noteId; float cents, d; };
    static constexpr int kMax = 16;
    Note notes[kMax];
    int nn = 0, count = 0;
    uint64_t hash = 1469598103934665603ull;
    for (const auto& v : voices)
    {
        if (! v.active || ! v.held || v.previewRemain >= 0) continue;
        ++count;
        bool have = false;
        for (int i = 0; i < nn; ++i) have = have || (notes[i].key == v.key && notes[i].noteId == v.noteId);
        if (have || nn >= kMax) continue;
        const int src = v.noteId >= 0 ? v.key : juce::jlimit (0, kNumKeys - 1, keySource[(size_t) v.key]);
        const float c = 1200.0f * std::log2 (slotHz[(size_t) src]) + (v.noteId >= 0 ? v.noteCents : p.tuning.cents[(size_t) v.key]);
        notes[nn++] = { v.key, v.noteId, c, 0.0f };
        hash = (hash ^ (uint64_t) (uint32_t) (v.key * 4096 + v.noteId + 1)) * 1099511628211ull;
        hash = (hash ^ (uint64_t) (int64_t) std::lround (c * 4.0f)) * 1099511628211ull;
    }
    hash = (hash ^ (uint64_t) count) * 1099511628211ull;
    if (hash == 0) hash = 1;
    if (hash != pureHash)
    {
        pureHash = hash;
        // the pure interval a pair is nearest to (cents, how consonant, how far off it may be and still be meant)
        struct Just { float cents, w, reach; };
        static const Just just[] { { 0.0f, 1.0f, 30.0f }, { 111.73f, 0.12f, 25.0f }, { 182.40f, 0.15f, 20.0f }, { 203.91f, 0.3f, 25.0f },
                                   { 266.87f, 0.12f, 18.0f }, { 315.64f, 0.6f, 40.0f }, { 386.31f, 0.8f, 45.0f }, { 498.04f, 0.9f, 45.0f },
                                   { 582.51f, 0.1f, 18.0f }, { 590.22f, 0.12f, 20.0f }, { 701.96f, 1.0f, 45.0f }, { 813.69f, 0.6f, 45.0f },
                                   { 884.36f, 0.7f, 40.0f }, { 968.83f, 0.2f, 20.0f }, { 996.09f, 0.25f, 22.0f }, { 1017.60f, 0.25f, 22.0f },
                                   { 1088.27f, 0.2f, 25.0f }, { 1200.0f, 1.0f, 30.0f } };
        struct Spring { int lo, hi; float e, w; };
        Spring springs[kMax * (kMax - 1) / 2];
        int ns = 0;
        for (int i = 0; i < nn; ++i)
            for (int j = i + 1; j < nn; ++j)
            {
                const int lo = notes[i].cents <= notes[j].cents ? i : j, hi = lo == i ? j : i;
                const float d = std::fmod (notes[hi].cents - notes[lo].cents, 1200.0f);
                float bestScore = 0.0f, e = 0.0f, w = 0.0f;
                for (const auto& jn : just)
                {
                    const float dev = jn.cents - d, score = jn.w * (1.0f - std::abs (dev) / jn.reach);
                    if (score > bestScore) { bestScore = score; e = dev; w = jn.w; }
                }
                if (bestScore > 0.0f) springs[ns++] = { lo, hi, e, w };
            }
        for (int it = 0; it < 24; ++it)
            for (int k = 0; k < nn; ++k)
            {
                float num = 0.0f, den = 0.06f;
                for (int s = 0; s < ns; ++s)
                {
                    const auto& sp = springs[s];
                    if (sp.hi == k)      { num += sp.w * (notes[sp.lo].d + sp.e); den += sp.w; }
                    else if (sp.lo == k) { num += sp.w * (notes[sp.hi].d - sp.e); den += sp.w; }
                }
                notes[k].d = num / den;
            }
        float mean = 0.0f;
        for (int k = 0; k < nn; ++k) mean += notes[k].d;
        if (nn > 0) mean /= (float) nn;
        for (auto& v : voices)
        {
            if (! v.active || ! v.held) continue;
            for (int k = 0; k < nn; ++k)
                if (notes[k].key == v.key && notes[k].noteId == v.noteId) { v.pureT = juce::jlimit (-60.0f, 60.0f, notes[k].d - mean); break; }
        }
    }
    // every pipe glides to its place (released ones keep theirs)
    const float k = 1.0f - std::exp (-(float) dt / juce::jmax (0.01f, p.live.pureTime / 3.0f));
    float mx = 0.0f;
    for (auto& v : voices)
    {
        if (! v.active) continue;
        v.pureC += k * (amt * v.pureT - v.pureC);
        mx = juce::jmax (mx, std::abs (v.pureC));
    }
    pureNow = mx;
}

// ---------------------------------------------------------------------------
// sympathetic pipes: a silent pipe rings (a resonator at its own pitch, at its own place) when a partial of a
// sounding pipe meets it. All of them listen to the same thing: the sum of the sounding pipes.
// ---------------------------------------------------------------------------
void OrganEngine::updateSympathy (const EngineParams& p) noexcept
{
    for (auto& s : symps) s.seen = false;
    const float level = p.live.sympLevel;
    if (level > 1.0e-5f && library != nullptr)
    {
        // the partials in the air (log2 Hz, strength)
        static constexpr int kParts = 120;
        float part[kParts], pw[kParts];
        int np = 0;
        std::array<uint64_t, kNumStops> sounding {};
        for (const auto& v : voices)
        {
            if (! v.active) continue;
            sounding[(size_t) v.stop] |= (uint64_t) 1 << v.key;
            if (! v.held || v.hzNow < 8.0f || v.gainLin < 1.0e-4f) continue;
            for (int n = 1; n <= 6 && np < kParts; ++n)
            {
                const float f = (float) n * v.hzNow;
                if (f > 0.4f * (float) sr) break;
                part[np] = std::log2 (f); pw[np] = juce::jmin (2.0f, v.gainLin) / (float) n; ++np;
            }
        }
        struct Cand { float w = 0.0f, hz = 0.0f; int stop = 0, key = 0; };
        std::array<Cand, kSymp> best {};
        if (np > 0)
            for (int s = 0; s < kNumStops; ++s)
                for (int k = 0; k < kNumKeys; ++k)
                {
                    if (((sounding[(size_t) s] >> k) & 1) || muted[(size_t) s][(size_t) k].load()) continue;
                    const float hz = pipeHzIdeal (s, k, p);
                    if (hz < 16.0f) continue;
                    const float l = std::log2 (hz);
                    float w = 0.0f;
                    for (int i = 0; i < np; ++i)
                    {
                        const float d = std::abs (part[i] - l) * 1200.0f;
                        if (d < 25.0f) w += pw[i] * (1.0f - d / 25.0f);
                    }
                    if (w <= best[kSymp - 1].w) continue;
                    int at = kSymp - 1;
                    while (at > 0 && best[(size_t) (at - 1)].w < w) { best[(size_t) at] = best[(size_t) (at - 1)]; --at; }
                    best[(size_t) at] = { w, hz, s, k };
                }
        const float r = std::exp (-1.0f / (juce::jlimit (0.03f, 6.0f, p.live.sympDecay) * 0.145f * (float) sr));   // (decay = -60 dB)
        for (const auto& c : best)
        {
            if (c.w <= 0.003f) continue;
            Symp* sy = nullptr;
            for (auto& x : symps) if (x.on && x.stop == c.stop && x.key == c.key) { sy = &x; break; }
            if (sy == nullptr) for (auto& x : symps) if (! x.on) { sy = &x; break; }
            if (sy == nullptr) continue;
            const bool fresh = ! sy->on;
            if (fresh) { *sy = Symp {}; sy->on = true; sy->stop = c.stop; sy->key = c.key; }
            sy->seen = true;
            sy->hz = c.hz;
            const float w0 = juce::MathConstants<float>::twoPi * c.hz / (float) sr;
            sy->b1 = 2.0f * r * std::cos (w0); sy->b2 = -r * r;
            // (1 - r) * 2 sin w: a partial exactly at the pipe's pitch comes out as loud as it went in
            // (several pipes share a pitch - the unisons and octaves of other stops - and add up: each one stays small)
            sy->gainT = 0.05f * level * juce::jmin (1.0f, c.w) * (1.0f - r) * 2.0f * std::sin (w0);
            const Pos pos = heardFrom (currentPosition (c.stop, c.key, p));
            float g[kMaxAmbiChannels];
            gainsFor (pos, g);
            const float dg = distanceGain (pos.dist, p.distAmount);
            for (int ch = 0; ch < kMaxAmbiChannels; ++ch) sy->g1[(size_t) ch] = g[ch] * dg;
            sy->send = distanceGain (pos.dist, p.distAmount * 0.35f);
            if (fresh) sy->g0 = sy->g1;
        }
    }
    for (auto& s : symps) if (s.on && ! s.seen) s.gainT = 0.0f;       // nothing excites it any more: it rings out
}

// ---------------------------------------------------------------------------
// the sounds of the living organ: the Kalkant's tread, the pipes that ring along
// ---------------------------------------------------------------------------
void OrganEngine::renderLive (float* const* outs, int n, const EngineParams& p) noexcept
{
    if (n <= 0) return;
    if (treadEnv > 1.0e-4f)
    {
        // the tread: a dull thump from the console (the bellows board and the air rushing in)
        float g[kMaxAmbiChannels];
        gainsFor ({ p.consoleAz, juce::jmax (-60.0f, p.consoleEl - 25.0f), 2.0f }, g);
        const float dec = std::exp (-1.0f / (0.11f * (float) sr)), lpA = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 140.0f / (float) sr);
        const float lvl = 0.5f * p.live.treadNoise;
        float* y = scratch.data();
        for (int i = 0; i < n; ++i)
        {
            treadLp += lpA * (rng.bi() - treadLp);
            treadLp2 += lpA * (treadLp - treadLp2);
            treadTone += (double) (36.0f + 46.0f * treadEnv) / sr;
            if (treadTone >= 1.0) treadTone -= 1.0;
            y[i] = lvl * treadEnv * (5.0f * treadLp2 + 0.5f * treadEnv * (float) std::sin (juce::MathConstants<double>::twoPi * treadTone));
            treadEnv *= dec;
        }
        if (lvl > 0.0f)
        {
            for (int c = 0; c < usedChannels; ++c)
                if (std::abs (g[c]) > 1.0e-6f) juce::FloatVectorOperations::addWithMultiply (outs[c], y, g[c], n);
            juce::FloatVectorOperations::addWithMultiply (roomSend[0].data() + roomOffset, y, 0.5f, n);
        }
    }
    if (! sympBusOn) return;
    int live = 0;
    float* y = scratch.data();
    const float* bus = sympBus.data();
    for (auto& s : symps)
    {
        if (! s.on) continue;
        float y1 = s.y1, y2 = s.y2, gain = s.gain;
        const float gStep = (s.gainT - gain) * 0.06f / (float) n;
        for (int i = 0; i < n; ++i)
        {
            gain += gStep;
            const float v = gain * bus[i] + s.b1 * y1 + s.b2 * y2;
            y2 = y1; y1 = v; y[i] = v;
        }
        s.y1 = y1; s.y2 = y2; s.gain = gain;
        for (int c = 0; c < usedChannels; ++c)
        {
            s.g0[(size_t) c] += 0.08f * (s.g1[(size_t) c] - s.g0[(size_t) c]);
            if (std::abs (s.g0[(size_t) c]) > 1.0e-6f) juce::FloatVectorOperations::addWithMultiply (outs[c], y, s.g0[(size_t) c], n);
        }
        juce::FloatVectorOperations::addWithMultiply (roomSend[0].data() + roomOffset, y, s.send, n);
        if (s.gainT <= 0.0f && std::abs (y1) + std::abs (y2) < 1.0e-6f) s.on = false;
        else ++live;
    }
    sympNow = live;
}

// ---------------------------------------------------------------------------
// Tonnetz-Wanderer: while the player holds keys, a walker steps through the lattice of fifths and major thirds,
// starting at the key struck last, and sounds every key it comes to
// ---------------------------------------------------------------------------
void OrganEngine::stepWalker (const EngineParams& p, int numSamples)
{
    const auto& lv = p.live;
    auto stillWanted = [this] (int key)
    {
        for (const auto& nn : netNotes) if (nn.key == key) return true;
        return keyHeld[(size_t) key] || keySustained[(size_t) key];
    };
    for (auto& nn : netNotes)
        if (nn.key >= 0 && (nn.left -= numSamples) <= 0)
        {
            const int key = nn.key;
            nn.key = -1;
            if (! stillWanted (key)) for (int s = 0; s < kNumStops; ++s) releaseStopKey (s, key);
        }
    bool anyHeld = false;
    for (int k = 0; k < kNumKeys; ++k) anyHeld = anyHeld || keyHeld[(size_t) k] || keySustained[(size_t) k];
    if (! (lv.netOn && anyHeld && ! freeMode && library != nullptr && lastDownKey >= 0))
    {
        if (netRunning) { netRunning = false; netKeyNow = -1; }       // (the last notes end with their own length)
        return;
    }
    static const double divQuarters[] { 0.0, 4.0, 2.0, 1.0, 0.5, 0.25, 1.0 / 3.0, 1.0 / 6.0 };
    const double q = divQuarters[juce::jlimit (0, 7, lv.netDiv)];
    const double stepSeconds = q > 0.0 ? q * 60.0 / juce::jmax (20.0, p.bpm) : 1.0 / juce::jmax (0.05, (double) lv.netRate);
    if (! netRunning || lastDownKey != netSeed)
    {
        // a new key: the walk starts there again
        netRunning = true;
        netSeed = netPrev = lastDownKey;
        netA = netB = netStep = 0; netLastDir = -1;
        netPhase = 0.0;
        netLastStep = q > 0.0 && p.playing ? std::floor (p.ppq / q) : -1.0e18;
    }
    bool step = false;
    if (q > 0.0 && p.playing)
    {
        const double idx = std::floor (p.ppq / q);
        if (idx != netLastStep) { step = true; netLastStep = idx; }
    }
    else if ((netPhase += (double) numSamples / sr / stepSeconds) >= 1.0) { netPhase -= std::floor (netPhase); step = true; }
    if (! step) return;

    // the six neighbours: a fifth up / down, a major third up / down, a minor third up / down
    static const int dA[6] { 1, -1, 0, 0, 1, -1 }, dB[6] { 0, 0, 1, -1, -1, 1 }, semis[6] { 7, -7, 4, -4, 3, -3 };
    auto randomDir = [this] { int d = (int) (netRng.next() % 6u); if (netLastDir >= 0 && d == (netLastDir ^ 1)) d = (d + 2) % 6; return d; };
    int dir = 0;
    switch (lv.netPath)
    {
        case 0: dir = 0; break;                                         // along the fifths (after eleven: the wolf)
        case 1: dir = (netStep & 1) ? 4 : 2; break;                     // a chain of thirds: major, minor, major ...
        case 2: dir = randomDir(); break;
        case 3:
        {
            // random, but homesick: of two ways it mostly takes the one that leads back
            const int d1 = randomDir(), d2 = randomDir();
            auto far = [&] (int d) { const int a = netA + dA[d], b = netB + dB[d]; return a * a + b * b + a * b; };
            dir = (far (d1) <= far (d2)) == (netRng.uni() < 0.75f) ? d1 : d2;
            break;
        }
        default:
        {
            // a spiral around the first key: 1 fifth, 1 third, 2 fifths back, 2 thirds back, 3 ...; then home again
            if (netStep >= 24) { netStep = 0; netA = netB = 0; netPrev = netSeed; }
            static const int turn[4] { 0, 2, 1, 3 };
            int leg = 0, len = 1, cnt = netStep;
            while (cnt >= len) { cnt -= len; ++leg; if (leg % 2 == 0) ++len; }
            dir = turn[leg % 4];
            break;
        }
    }
    netA += dA[dir]; netB += dB[dir]; ++netStep; netLastDir = dir;
    const int seedMidi = kPhysKeys[(size_t) netSeed], want = kPhysKeys[(size_t) juce::jlimit (0, kNumKeys - 1, netPrev)] + semis[dir];
    const int pc = (((seedMidi + 7 * netA + 4 * netB) % 12) + 12) % 12;
    // the key with that note: where the step leads, but never far from where the walk began
    int best = -1; float bestCost = 1.0e9f;
    for (int k = 0; k < kNumKeys; ++k)
    {
        const int m = kPhysKeys[(size_t) k];
        if (m % 12 != pc) continue;
        const float cost = (float) std::abs (m - want) + 0.35f * (float) std::abs (m - seedMidi);
        if (cost < bestCost) { bestCost = cost; best = k; }
    }
    if (best < 0) return;
    netPrev = best;
    netKeyNow = best;
    const int length = juce::jmax (64, (int) ((double) juce::jlimit (0.05f, 8.0f, lv.netGate) * stepSeconds * sr));
    bool sounding = keyHeld[(size_t) best] || keySustained[(size_t) best];
    for (auto& nn : netNotes) if (nn.key == best) { nn.left = juce::jmax (nn.left, length); sounding = true; }
    if (sounding) return;                                  // the player (or an earlier step) already sounds it
    NetNote* slot = nullptr;
    for (auto& nn : netNotes) if (nn.key < 0) { slot = &nn; break; }
    if (slot == nullptr)
    {
        // all eight are ringing: the one with the least time left makes room
        slot = &netNotes[0];
        for (auto& nn : netNotes) if (nn.left < slot->left) slot = &nn;
        const int old = slot->key;
        slot->key = -1;
        if (! stillWanted (old)) for (int s = 0; s < kNumStops; ++s) releaseStopKey (s, old);
    }
    slot->key = best; slot->left = length;
    keyVelocity[(size_t) best] = lastVelocity;
    for (int s = 0; s < kNumStops; ++s)
        if (p.stopOn[(size_t) s]) { releaseStopKey (s, best); startVoice (s, best); }
}
} // namespace rysum
