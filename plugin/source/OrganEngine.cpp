#include "OrganEngine.h"
#include "Kernels.h"
#include "SamplePitch.h"

namespace rysum
{
static constexpr float kSpeedOfSound = 343.0f;
static constexpr float kChaosLevel =
    0.009f; // wind model 2: level of the whistles / hiss against the pipe samples
static constexpr float kFdnLevel =
    6.03f; // late reverb FDN: level against the convolution tail (same room, same energy)

// ---------------------------------------------------------------------------
// mixing: one signal into many channels with per-channel gains (the inner loop of every pipe). Four
// channels per pass, so the signal is read once for four outputs (Kernels.h: AVX2 / FMA where the
// CPU has it).
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#define RYSUM_RESTRICT __restrict
#else
#define RYSUM_RESTRICT __restrict__
#endif

static inline void mix1(float *RYSUM_RESTRICT o, const float *RYSUM_RESTRICT x,
                        const float *RYSUM_RESTRICT r, float a, float d, int n) noexcept
{
    if (r != nullptr)
        for (int i = 0; i < n; ++i)
            o[i] += x[i] * a + r[i] * d;
    else
        for (int i = 0; i < n; ++i)
            o[i] += x[i] * a;
}

/** dst_c[i] += sum_j y_j[i] * e_j[c] for four signals y_j (e_j: their channel gains) */
static void mixGroup4(float *const *dst, int numCh, float *const *y, const float *const *e,
                      int n) noexcept
{
    static const kern::Table &K = kern::get();
    int c = 0;
    for (; c + 4 <= numCh; c += 4)
    {
        float g[16];
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                g[4 * j + k] = e[j][c + k];
        K.mix4x4(dst[c], dst[c + 1], dst[c + 2], dst[c + 3], y[0], y[1], y[2], y[3], g, n);
    }
    for (; c < numCh; ++c)
    {
        float *RYSUM_RESTRICT o = dst[c];
        const float *RYSUM_RESTRICT y0 = y[0];
        const float *RYSUM_RESTRICT y1 = y[1];
        const float *RYSUM_RESTRICT y2 = y[2];
        const float *RYSUM_RESTRICT y3 = y[3];
        const float a = e[0][c], b = e[1][c], cc = e[2][c], d = e[3][c];
        for (int i = 0; i < n; ++i)
            o[i] += y0[i] * a + y1[i] * b + y2[i] * cc + y3[i] * d;
    }
}

/** dst_c[off + i] += x[i] * a_c + ramp[i] * d_c   (ramp may be null = constant gains; d may be null
   too). Channels with negligible gains are left out. */
static void mixInto(float *const *dst, int off, int numCh, const float *x, const float *ramp,
                    const float *a, const float *d, int n, float eps = 1.0e-9f) noexcept
{
    if (n <= 0)
        return;
    static const kern::Table &K = kern::get();
    bool useRamp = ramp != nullptr && d != nullptr;
    if (useRamp)
    {
        bool any = false;
        for (int c = 0; c < numCh; ++c)
            any = any || std::abs(d[c]) > 1.0e-12f;
        useRamp = any;
    }
    int c = 0;
    while (c + 4 <= numCh)
    {
        const bool live =
            std::abs(a[c]) > eps || std::abs(a[c + 1]) > eps || std::abs(a[c + 2]) > eps ||
            std::abs(a[c + 3]) > eps ||
            (useRamp && (std::abs(d[c]) > 1.0e-12f || std::abs(d[c + 1]) > 1.0e-12f ||
                         std::abs(d[c + 2]) > 1.0e-12f || std::abs(d[c + 3]) > 1.0e-12f));
        if (live)
        {
            if (useRamp)
                K.mix4(dst[c] + off, dst[c + 1] + off, dst[c + 2] + off, dst[c + 3] + off, x, ramp,
                       a + c, d + c, n);
            else
                K.mix4c(dst[c] + off, dst[c + 1] + off, dst[c + 2] + off, dst[c + 3] + off, x,
                        a + c, n);
        }
        c += 4;
    }
    for (; c < numCh; ++c)
        if (std::abs(a[c]) > eps || (useRamp && std::abs(d[c]) > 1.0e-12f))
            mix1(dst[c] + off, x, useRamp && std::abs(d[c]) > 1.0e-12f ? ramp : nullptr, a[c],
                 useRamp ? d[c] : 0.0f, n);
}

OrganEngine::OrganEngine()
{
    for (int r = 0; r < kNumRooms; ++r)
    {
        drawnPlans[(size_t)r][0] = drawnPlans[(size_t)r][1] = defaultDrawnPlan();
        drawnPlanActive[(size_t)r] = 0;
    }
    voices.resize((size_t)kMaxVoices);
    noise.resize((size_t)kMaxNoiseVoices);
    for (int k = 0; k < kNumKeys; ++k)
    {
        keySource[(size_t)k] = k;
        slotHz[(size_t)k] = (float)meantoneHz(kPhysKeys[(size_t)k]);
    }
    for (int s = 0; s < kNumStops; ++s)
    {
        std::array<Pos, kNumKeys> pos{};
        generateLayout(defaultLayout(s), s, pos);
        for (int k = 0; k < kNumKeys; ++k)
        {
            baseAz[(size_t)s][(size_t)k] = pos[(size_t)k].az;
            baseEl[(size_t)s][(size_t)k] = pos[(size_t)k].el;
            baseDist[(size_t)s][(size_t)k] = pos[(size_t)k].dist;
            muted[(size_t)s][(size_t)k] = false;
            beatTurn[(size_t)s][(size_t)k] = 0.0f;
            wear[(size_t)s][(size_t)k] = 0.0f;
            pipeLen[(size_t)s][(size_t)k] = 1.0f;
        }
    }
    uint32_t seed = 1;
    for (auto &v : voices)
    {
        v.deg.reset(seed++ * 7919u);
        XorShift r(seed * 31u);
        v.wobble.init(r);
        v.breath.init(r);
        v.breath.f1 *= 6.0f;
        v.breath.f2 *= 4.0f;
        v.breath.f3 *= 2.0f;
        v.grainRng = XorShift(seed * 104729u + 17u);
    }
}

void OrganEngine::prepare(double sampleRate, int maxBlockSize)
{
    sr = sampleRate > 0 ? sampleRate : 48000.0;
    maxBlock = juce::jmax(1, maxBlockSize);
    scratch.assign((size_t)maxBlock, 0.0f);
    bodyMode.fill(-1);
    bodySamples = 0;
    bodyStepCount = 0;
    numImpacts = 0; // (the same paths after every start)
    for (auto &v : impactVoices)
        v.on = false;
    impactRng = XorShift(4711u);
    for (auto &b : bodyOn)
        b = false;
    scratchRamp.assign((size_t)maxBlock, 0.0f);
    hidBuf.assign(2 * (size_t)maxBlock, 0.0f);
    nearL.assign((size_t)maxBlock, 0.0f);
    nearR.assign((size_t)maxBlock, 0.0f);
    nearUsed = false;
    for (auto &r : roomSend)
        r.assign((size_t)maxBlock, 0.0f);
    gust.assign((size_t)(maxBlock / 16 + 4), 0.0f);
    trem.assign((size_t)(maxBlock / 16 + 4), 0.0f);
    chunkMidi.ensureSize(8192);
    for (auto &v : voices)
        v.active = false;
    for (auto &n : noise)
        n.active = false;
    // effects
    echoBuf.assign((size_t)(16.5 * sr) + (size_t)maxBlock, 0.0f);
    echoWrite = 0;
    echoDelay = -1.0;
    echoLp.fill(0.0f);
    fxScratch.assign((size_t)maxBlock * 12, 0.0f);
    // simulated room (all memory here, nothing is allocated while playing)
    erSlots.assign((size_t)kErSlots, ErSlot{});
    for (int ri = 0; ri < kNumRooms; ++ri)
    {
        erListener[(size_t)ri] = listenerPos(roomsEr[(size_t)ri]);
        erGeo[(size_t)ri] = planGeo(roomsEr[(size_t)ri]);
    }
    for (auto &w : erWork)
    {
        const size_t slice = (size_t)juce::jmax(maxBlock, kSubBlock);
        w.y.assign(8 * slice, 0.0f);
        w.bus.assign(20 * slice, 0.0f);
        w.paths.assign((size_t)kPlanPaths, PlanPath{});
        w.cands.assign((size_t)kPlanCand, PlanImage{});
        w.active = 0;
        w.used = false;
    }
    // spatial degradation and the grain cloud (all memory here)
    eroder.prepare(sr);
    // photographic operations, ice walls, difference tones
    dexFx.prepare(sr, 8);
    expoFx.prepare(sr);
    iceHi.reset();
    iceLo.reset();
    iceWasOn = false;
    iceBus.assign((size_t)20 * (size_t)maxBlock, 0.0f);
    for (auto &g : ghosts)
        g = Ghost{};
    {
        int rs = 1;
        while (rs < (int)(0.16 * sr) + maxBlock + 8)
            rs <<= 1;
        ringSize = rs;
        ringMask = (uint32_t)(rs - 1);
        cloudData.assign((size_t)kCloudRings * (size_t)ringSize, 0.0f);
        ringPos.fill(0);
        ringOwner.fill(0);
        ringRefs.fill(0);
        for (auto &g : grains)
            g = CloudGrain{};
    }
    erRing.assign((size_t)kErSlots * (size_t)kErRing, 0.0f);
    for (auto &v : voices)
        v.erSlot = -1;
    for (auto &n : noise)
        n.erSlot = -1;
    inRing.assign((size_t)2 * (size_t)kInRing, 0.0f);
    for (auto &is : inSrc)
    {
        is = InSrc{};
    }
    inRunning = false;
    inGate = inLevelSm = inEnv = inFollowVal = 0.0f;
    for (auto &R : rt)
    {
        R.tail.prepare(sr);
        R.tailGain = 0.0f;
        R.tailWasOn = false;
        R.fdnBuf.assign((size_t)kFdnLines * (size_t)kFdnLen, 0.0f);
        R.fdnPre.assign((size_t)kFdnLen, 0.0f);
        R.convRingOut = R.fdnRingOut = 0;
        R.send.assign((size_t)juce::jmax(maxBlock, kSubBlock) + 16, 0.0f);
        R.running = false;
        resetFdn(R);
    }
    room0In.assign((size_t)juce::jmax(maxBlock, kSubBlock) + 16, 0.0f);
    lucier.prepare(sr);
    lucIn.assign((size_t)juce::jmax(maxBlock, kSubBlock) + 16, 0.0f);
    lucWet.assign(lucIn.size(), 0.0f);
    lucRunning = false;
    lucRing = 0;
    lucDry = 1.0f;
    lucWetGain = 0.0f;
    zeroIn.assign((size_t)juce::jmax(maxBlock, kSubBlock) + 16, 0.0f);
    // living organ
    sympBus.assign((size_t)juce::jmax(maxBlock, kSubBlock) + 16, 0.0f);
    for (auto &s : symps)
        s = Symp{};
    for (auto &nn : netNotes)
        nn = NetNote{};
    netRunning = false;
    netKeyNow = -1;
    bellowsP = bellowsV = 1.0f;
    bellowsRising = false;
    treadEnv = 0.0f;
    {
        // FDN mixing matrix: a fixed random orthogonal matrix (Gram-Schmidt) instead of a Hadamard
        // matrix (that one left isolated modes around 1.5-2 kHz). Together with a slow, small
        // modulation of every delay (different rate per line) the modes are smeared out.
        XorShift r(3);
        std::array<std::array<double, kFdnLines>, kFdnLines> m{};
        for (auto &row : m)
            for (auto &x : row)
                x = r.bi();
        for (int i = 0; i < kFdnLines; ++i)
        {
            for (int j = 0; j < i; ++j)
            {
                double dot = 0;
                for (int k = 0; k < kFdnLines; ++k)
                    dot += m[(size_t)i][(size_t)k] * m[(size_t)j][(size_t)k];
                for (int k = 0; k < kFdnLines; ++k)
                    m[(size_t)i][(size_t)k] -= dot * m[(size_t)j][(size_t)k];
            }
            double len = 0;
            for (int k = 0; k < kFdnLines; ++k)
                len += m[(size_t)i][(size_t)k] * m[(size_t)i][(size_t)k];
            len = std::sqrt(len);
            for (int k = 0; k < kFdnLines; ++k)
                m[(size_t)i][(size_t)k] /= len;
        }
        for (int i = 0; i < kFdnLines; ++i)
            for (int k = 0; k < kFdnLines; ++k)
                fdnMatrix[(size_t)i][(size_t)k] = (float)m[(size_t)i][(size_t)k];
        for (int j = 0; j < kFdnLines; ++j)
        {
            const double rate = 0.3 + 0.9 * r.uni(),
                         ph = juce::MathConstants<double>::twoPi * r.uni();
            for (auto &R : rt)
            {
                R.fdnModC[(size_t)j] = (float)std::cos(ph);
                R.fdnModS[(size_t)j] = (float)std::sin(ph);
            }
            const double w = juce::MathConstants<double>::twoPi * rate / sr;
            fdnModStepC[(size_t)j] = (float)std::cos(w);
            fdnModStepS[(size_t)j] = (float)std::sin(w);
        }
    }
    numScheduled = 0;
    {
        XorShift r(77);
        for (size_t i = 0; i < leaks.size(); ++i)
        {
            auto &L = leaks[i];
            L.pos = {180.0f * r.bi(), -5.0f + 50.0f * r.uni(), 3.0f + 6.0f * r.uni()};
            L.freqMul = 0.7f + 0.7f * r.uni();
            L.dir = r.uni() < 0.5f ? -1.0f : 1.0f;
            L.rng = XorShift((uint32_t)(i * 7919 + 13));
            L.s1 = L.s2 = L.amp = 0.0f;
        }
    }
    keyHeld.fill(false);
    keySustained.fill(false);
    firstBlock = true;
}

void OrganEngine::setLibrary(std::shared_ptr<const SampleLibrary> lib)
{
    for (auto &v : voices)
    {
        v.active = false;
        releaseSlot(v.erSlot);
    }
    for (auto &n : noise)
    {
        n.active = false;
        releaseSlot(n.erSlot);
    }
    library = std::move(lib);
    windPos = 0.0;
    numScheduled = 0; // (audio thread: no allocation here)
}

// ---------------------------------------------------------------------------
// effects: placed sounds, tracker path, scheduling
// ---------------------------------------------------------------------------
const std::vector<NoiseSample> &OrganEngine::customPool() const noexcept
{
    // no own sounds: the recorded clicks (the "other" ones are the most varied)
    if (library == nullptr)
        return noSounds;
    if (!library->custom.empty())
        return library->custom;
    if (!library->clicksDown.empty())
        return library->clicksDown;
    if (!library->clicksOther.empty())
        return library->clicksOther;
    return library->stopPulls;
}

void OrganEngine::placedGains(NoiseVoice &nv) noexcept
{
    gainsFor(nv.where, nv.g.data());
    const float dg = distanceGain(nv.where.dist, distAmountNow);
    for (auto &x : nv.g)
        x *= dg;
    const double a = juce::degreesToRadians((double)(roomFollowNow ? nv.where.az : roomBaseAzNow));
    nv.sendW[0] = 1.0f;
    for (int m = 1; 2 * m < kMaxRoomSends; ++m)
    {
        nv.sendW[(size_t)(2 * m - 1)] = (float)std::cos(m * a);
        nv.sendW[(size_t)(2 * m)] = (float)std::sin(m * a);
    }
}

void OrganEngine::spawnSound(const Sample &s, float gain, float tone, double rate, Pos pos,
                             float azVel, float send)
{
    if (gain <= 0.00001f || s.size() < 4)
        return;
    NoiseVoice *nv = nullptr;
    for (auto &n : noise)
        if (!n.active)
        {
            nv = &n;
            break;
        }
    if (nv == nullptr)
        nv = &noise[(size_t)(rng.next() % noise.size())];
    if (nv->active)
        releaseSlot(nv->erSlot);
    nv->active = true;
    nv->erSlot = roomOnNow ? allocSlot(-1, (int)(nv - noise.data())) : -1;
    nv->data = s.data.data();
    nv->length = s.size();
    nv->pos = 0.0;
    nv->where = pos;
    nv->rate = s.sampleRate / sr * rate;
    nv->gain = gain;
    nv->send = send;
    nv->lp = 0.0f;
    nv->lpA = juce::jlimit(0.02f, 1.0f, tone);
    nv->azVel = azVel;
    nv->resN = 0;
    nv->resTail = 0;
    nv->where.dist = juce::jlimit(kMinDist, kMaxDist, pos.dist);
    placedGains(*nv);
}

void OrganEngine::schedule(const Scheduled &e)
{
    if (numScheduled < (int)scheduled.size())
        scheduled[(size_t)numScheduled++] = e;
}

void OrganEngine::customSounds(int key, float velocity, const EngineParams &p)
{
    const auto &pool = customPool();
    const auto &fx = p.fx;
    if (pool.empty() || fx.custLevel <= 0.0001f)
        return;
    const int copies = 1 + (int)std::floor(fx.custScatter * (1.0f + 4.0f * rng.uni()));
    std::array<int, kNumStops> pulled{};
    int nPulled = 0;
    for (int s = 0; s < kNumStops; ++s)
        if (p.stopOn[(size_t)s])
            pulled[(size_t)nPulled++] = s;
    for (int c = 0; c < copies; ++c)
    {
        int idx = (int)(rng.uni() * (float)pool.size());
        if (pool.size() > 1 && idx == lastCustom)
            idx = (idx + 1) % (int)pool.size();
        idx = juce::jlimit(0, (int)pool.size() - 1, idx);
        lastCustom = idx;
        const auto *smp = pool[(size_t)idx].sample.get();
        if (smp == nullptr)
            continue;
        const float norm = pool[(size_t)idx].norm;

        Pos pos{p.consoleAz, p.consoleEl, 2.0f};
        switch (fx.custPlace)
        {
        case 0: // at the pipe of a pulled stop (or the console when none is pulled)
            if (nPulled > 0)
                pos = currentPosition(pulled[(size_t)(rng.next() % (uint32_t)nPulled)], key, p);
            pos.az = wrapAz(pos.az + 35.0f * fx.custScatter * rng.bi());
            break;
        case 1:
            pos = {180.0f * rng.bi(), -10.0f + 70.0f * rng.uni(), 2.5f + 9.0f * rng.uni()};
            break;
        case 2:
            break;
        default:
            pos = {180.0f * rng.bi(), 30.0f * rng.uni(), 4.0f + 3.0f * rng.uni()};
            break;
        }
        float azVel = fx.custWander * (rng.uni() < 0.5f ? -1.0f : 1.0f) * (0.6f + 0.8f * rng.uni());
        if (fx.custPlace == 3 && std::abs(azVel) < 1.0f)
            azVel = (rng.uni() < 0.5f ? -1.0f : 1.0f) * 90.0f;
        const double rate = std::exp2((double)(kPhysKeys[(size_t)key] - 60) / 12.0 * fx.custPitch) *
                            (0.97 + 0.06 * rng.uni());
        const float gain = fx.custLevel * norm * clickCurve(velocity) * 0.7f *
                           (c == 0 ? 1.0f : 0.45f + 0.45f * rng.uni());
        const int delay = c == 0 ? 0 : (int)(rng.uni() * (0.05 + 0.75 * fx.custScatter) * sr);
        if (delay <= 0)
            spawnSound(*smp, gain, clickTone(velocity), rate, pos, azVel, 0.6f);
        else
        {
            Scheduled e;
            e.delay = delay;
            e.sample = smp;
            e.gain = gain;
            e.tone = clickTone(velocity);
            e.rate = rate;
            e.pos = pos;
            e.azVel = azVel;
            e.send = 0.6f;
            schedule(e);
        }
    }
}

void OrganEngine::trackerPath(int key, float velocity, const EngineParams &p)
{
    const auto &fx = p.fx;
    if (fx.pathLevel <= 0.0001f || library == nullptr)
        return;
    const auto &pool = !library->clicksDown.empty() ? library->clicksDown : library->clicksOther;
    if (pool.empty())
        return;
    std::array<int, kNumStops> pulled{};
    int nPulled = 0;
    for (int s = 0; s < kNumStops; ++s)
        if (p.stopOn[(size_t)s])
            pulled[(size_t)nPulled++] = s;
    const int targets = juce::jmin(3, juce::jmax(1, nPulled));
    const int ticks = juce::jlimit(2, 16, fx.pathTicks);
    const Pos console{p.consoleAz, p.consoleEl, 2.0f};
    for (int t = 0; t < targets; ++t)
    {
        const Pos dst = nPulled > 0
                            ? currentPosition(pulled[(size_t)((t * nPulled) / targets)], key, p)
                            : Pos{0.0f, 20.0f, 6.0f};
        const float daz = wrapAz(dst.az - console.az);
        for (int i = 1; i <= ticks; ++i)
        {
            const float f = (float)i / (float)ticks;
            const float bulge = std::sin(juce::MathConstants<float>::pi * f);
            Pos q{wrapAz(console.az + daz * f + fx.pathSpread * bulge * rng.bi()),
                  juce::jlimit(-90.0f, 90.0f,
                               console.el + (dst.el - console.el) * f +
                                   0.5f * fx.pathSpread * bulge * rng.bi()),
                  console.dist + (dst.dist - console.dist) * f};
            const auto &ns = pool[(size_t)(rng.next() % (uint32_t)pool.size())];
            const auto &smp = *ns.sample;
            const float gain = fx.pathLevel * ns.norm * clickCurve(velocity) * 0.25f *
                               (0.45f + 0.55f * f) / std::sqrt((float)targets);
            const double rate = 1.7 + 1.5 * rng.uni(); // small, high ticks
            const int delay = (int)(fx.pathTime * sr * f * (0.85f + 0.3f * rng.uni()));
            if (delay <= 0)
                spawnSound(smp, gain, clickTone(velocity), rate, q, 0.0f, 0.3f);
            else
            {
                Scheduled e;
                e.delay = delay;
                e.sample = &smp;
                e.gain = gain;
                e.tone = clickTone(velocity);
                e.rate = rate;
                e.pos = q;
                e.send = 0.3f;
                schedule(e);
            }
        }
    }
}

void OrganEngine::requestPreview(int stop, int key)
{
    const auto scope = previewFifo.write(1);
    if (scope.blockSize1 > 0)
        previewQueue[(size_t)scope.startIndex1] = {stop, key};
    else if (scope.blockSize2 > 0)
        previewQueue[(size_t)scope.startIndex2] = {stop, key};
}

Pos OrganEngine::currentPosition(int stop, int key, const EngineParams &p) const noexcept
{
    // in the room modes the pipe is a body in the Saal: seen from the usual listening place (no
    // orbit, no shaping)
    if (isRoomMotion(p.motionMode[(size_t)stop]))
    {
        Vec3f b;
        if (bodyAt(stop, key, b))
        {
            const int ri = juce::jlimit(0, kNumRooms - 1, p.stopRoom[(size_t)stop]);
            Pos q = seenFrom(
                {homeX[(size_t)ri].load(), homeY[(size_t)ri].load(), homeZ[(size_t)ri].load()}, b);
            q.dist = juce::jlimit(kMinDist, kMaxDist, q.dist);
            return q;
        }
    }
    Pos base{baseAz[(size_t)stop][(size_t)key].load(), baseEl[(size_t)stop][(size_t)key].load(),
             baseDist[(size_t)stop][(size_t)key].load()};
    if ((Motion)p.motionMode[(size_t)stop] ==
        Motion::Beat) // every pipe its own turn, as fast as it beats
        return applyMotion(base, Motion::Rotation,
                           (double)beatTurn[(size_t)stop][(size_t)key].load() / 360.0,
                           p.motionDepth[(size_t)stop], (uint32_t)(stop * 101 + key * 7 + 1),
                           shapeFor(stop, key, p));
    return applyMotion(base, (Motion)p.motionMode[(size_t)stop], motionPhase[(size_t)stop].load(),
                       p.motionDepth[(size_t)stop], (uint32_t)(stop * 101 + key * 7 + 1),
                       shapeFor(stop, key, p));
}

MotionShape OrganEngine::shapeFor(int stop, int key, const EngineParams &p) const noexcept
{
    MotionShape sh;
    sh.ellipse = p.ellipse[(size_t)stop];
    sh.ellipseDir = p.ellipseDir[(size_t)stop];
    sh.ellipseTilt = p.ellipseTilt[(size_t)stop];
    sh.ellipseLobes = p.ellipseLobes[(size_t)stop];
    sh.path = &getPath(stop);
    sh.pathSpread = p.pathSpread[(size_t)stop];
    sh.keyFrac = (float)key / (float)kNumKeys;
    return sh;
}

void OrganEngine::setPath(int stop, const MotionPath &path) noexcept
{
    if (stop < 0 || stop >= kNumStops)
        return;
    const int next = 1 - pathActive[(size_t)stop].load();
    paths[(size_t)stop][(size_t)next] = path;
    pathActive[(size_t)stop] = next;
}

void OrganEngine::hardJumpGains(const Voice &v, const EngineParams &p, double phase, float *g,
                                float *gh) const noexcept
{
    const Pos base{baseAz[(size_t)v.stop][(size_t)v.key].load(),
                   baseEl[(size_t)v.stop][(size_t)v.key].load(),
                   baseDist[(size_t)v.stop][(size_t)v.key].load()};
    Pos pos = applyMotion(base, Motion::Jumps, phase, p.motionDepth[(size_t)v.stop],
                          (uint32_t)(v.stop * 101 + v.key * 7 + 1), shapeFor(v.stop, v.key, p));
    pos.az = wrapAz(pos.az + v.pmAz);
    pos.el = juce::jlimit(-90.0f, 90.0f, pos.el + v.pmEl);
    pos.dist = juce::jlimit(kMinDist, kMaxDist, pos.dist * v.pmDist);
    pos = heardFrom(pos);
    const float k = v.gainLin * distanceGain(pos.dist, p.distAmount) * v.cloudDry;
    gainsFor(pos, g);
    for (int c = 0; c < kMaxAmbiChannels; ++c)
        g[c] *= k;
    if (gh != nullptr)
    {
        Pos ph = pos;
        ph.az = wrapAz(pos.az + p.fx.partSpread * v.partSide + (float)partRot);
        gainsFor(ph, gh);
        for (int c = 0; c < kMaxAmbiChannels; ++c)
            gh[c] *= k;
    }
}

void OrganEngine::gainsFor(Pos pos, float *out) const noexcept
{
    std::fill(out, out + kMaxAmbiChannels, 0.0f);
    if (gridN > 0)
    {
        // "Richtungsraster": the direction snaps to the nearest of a few grid directions
        const float a = juce::degreesToRadians(pos.az), e = juce::degreesToRadians(pos.el);
        const float x = -std::sin(a) * std::cos(e), y = std::cos(a) * std::cos(e), z = std::sin(e);
        int best = 0;
        float bd = -2.0f;
        for (int i = 0; i < gridN; ++i)
        {
            const float d = x * gridX[(size_t)i] + y * gridY[(size_t)i] + z * gridZ[(size_t)i];
            if (d > bd)
            {
                bd = d;
                best = i;
            }
        }
        pos.az = gridAz[(size_t)best];
        pos.el = gridEl[(size_t)best];
    }
    if (order == 0)
        encodeStereo(pos.az, out);
    else
        encodeAmbiX(pos.az, pos.el, order, out);
}

PipeSoundParams OrganEngine::pipeSndFor(const PipeSoundParams &b, const Voice &v) noexcept
{
    if (!v.pmNew)
        return b;
    const auto &m = v.pmx;
    auto s = b;
    if (m[tgtAttack] != 0.0f)
        s.attGain = juce::jlimit(-1.0f, 1.0f, b.attGain + m[tgtAttack]);
    if (m[tgtAttSharp] != 0.0f)
        s.attSharp = juce::jlimit(0.0f, 1.0f, b.attSharp + m[tgtAttSharp]);
    if (m[tgtAttIso] != 0.0f)
        s.attIso = juce::jlimit(0.0f, 1.0f, b.attIso + m[tgtAttIso]);
    if (m[tgtAttRep] != 0.0f)
        s.attRep = juce::jlimit(0.0f, 1.0f, b.attRep + m[tgtAttRep]);
    if (m[tgtPuff] != 0.0f)
        s.puff = juce::jlimit(0.0f, 1.0f, b.puff + m[tgtPuff]);
    if (m[tgtHiFreq] != 0.0f)
        s.hiFreq = juce::jlimit(
            500.0f, 16000.0f, b.hiFreq * std::exp2(4.0f * juce::jlimit(-2.0f, 2.0f, m[tgtHiFreq])));
    if (m[tgtHiGain] != 0.0f)
        s.hiGain = juce::jlimit(-24.0f, 18.0f, b.hiGain + 24.0f * m[tgtHiGain]);
    if (m[tgtHiShape] != 0.0f)
        s.hiShape = juce::jlimit(-1.0f, 1.0f, b.hiShape + m[tgtHiShape]);
    return s;
}

GlitchParams OrganEngine::glitchFor(const GlitchParams &b, const Voice &v) noexcept
{
    if (!v.pmNew)
        return b;
    const auto &m = v.pmx;
    auto g = b;
    g.stutter = juce::jlimit(0.0f, 1.0f, b.stutter + m[tgtStutter]);
    g.splice = juce::jlimit(0.0f, 1.0f, b.splice + m[tgtSplice]);
    g.zero = juce::jlimit(0.0f, 1.0f, b.zero + m[tgtZero]);
    g.clip = juce::jlimit(0.0f, 1.0f, b.clip + m[tgtClip]);
    return g;
}

CloudParams OrganEngine::cloudFor(const CloudParams &b, const Voice &v) noexcept
{
    if (!v.pmNew)
        return b;
    const auto &m = v.pmx;
    auto c = b;
    if (m[tgtCloudMix] != 0.0f)
        c.mix = juce::jlimit(0.0f, 1.0f, b.mix + m[tgtCloudMix]);
    if (m[tgtCloudDens] != 0.0f)
        c.density = juce::jlimit(
            1.0f, 200.0f, b.density * std::exp2(3.0f * juce::jlimit(-2.0f, 2.0f, m[tgtCloudDens])));
    if (m[tgtCloudSpread] != 0.0f)
        c.spread = juce::jlimit(0.0f, 180.0f, b.spread + 180.0f * m[tgtCloudSpread]);
    if (m[tgtCloudSize] != 0.0f)
        c.sizeMs = juce::jlimit(
            10.0f, 500.0f, b.sizeMs * std::exp2(3.0f * juce::jlimit(-2.0f, 2.0f, m[tgtCloudSize])));
    if (m[tgtCloudScatter] != 0.0f)
        c.scatterMs = juce::jlimit(0.0f, 150.0f, b.scatterMs + 150.0f * m[tgtCloudScatter]);
    return c;
}

namespace
{
/** adds the time a scope took to a stage's sum (only while measuring) */
struct ProfScope
{
    ProfScope(bool on, double &sum) : acc(on ? &sum : nullptr),
                                      t0(on ? juce::Time::getMillisecondCounterHiRes() : 0.0)
    {
    }
    ~ProfScope()
    {
        if (acc != nullptr)
            *acc += juce::Time::getMillisecondCounterHiRes() - t0;
    }
    double *acc;
    double t0;
};
} // namespace

/** direction gains, distance level, air absorption, room send and Doppler of one voice for this
 * block */
void OrganEngine::updateVoiceSpatial(Voice &v, const EngineParams &p, double blockSeconds) noexcept
{
    double profT = profOn ? juce::Time::getMillisecondCounterHiRes() : 0.0;
    auto profMark = [&](int stage)
    {
        if (!profOn)
            return;
        const double now = juce::Time::getMillisecondCounterHiRes();
        prof[(size_t)stage] += now - profT;
        profT = now;
    };
    // per-pipe envelopes
    for (int j = 0; j < kNumEnvs; ++j)
    {
        auto &e = v.menv[(size_t)j];
        if (!v.held && e.gate)
            e.releaseGate();
        e.advance(p.mod.env[(size_t)j], (float)blockSeconds);
    }
    // per-pipe modulation targets
    std::array<float, kNumPipeTargets> pm{};
    bool anyMod = false;
    for (const auto &r : p.mod.routes)
        if (r.src >= 0 && r.target >= 0 && r.target < kNumPipeTargets && r.amount != 0.0f)
        {
            pm[(size_t)r.target] += r.amount * shapeSource(r.src, voiceSource(v, r.src, p), r.uni);
            anyMod = true;
        }

    Pos pos = currentPosition(v.stop, v.key, p);
    if (anyMod)
    {
        pos.az = wrapAz(pos.az + 180.0f * pm[tgtAz]);
        pos.el = juce::jlimit(-90.0f, 90.0f, pos.el + 90.0f * pm[tgtEl]);
        const float dm = std::exp2(3.0f * juce::jlimit(-2.0f, 2.0f, pm[tgtDist]));
        pos.dist = juce::jlimit(kMinDist, kMaxDist, pos.dist * dm);
    }
    const Pos homePos = pos;
    pos = heardFrom(pos); // the listener walks: the pipe as heard from there
    profMark(8);
    {
        // Wachsen & Brechen: the pitch follows the pipe's length (a glide of a few hundredths of
        // a second, so a pipe that breaks while it sounds jumps without a click)
        const float lc = -1200.0f * std::log2(juce::jlimit(0.2f, 4.0f, pipeLen[(size_t)v.stop][(size_t)v.key].load()));
        if (blockSeconds != spatialDt)
        {
            spatialDt = blockSeconds;
            spatialLenK = 1.0f - (float)std::exp(-blockSeconds / 0.03);
            spatialCrk = (float)std::exp(-blockSeconds / 0.15);
        }
        v.lenCents = v.fresh ? lc : v.lenCents + (lc - v.lenCents) * spatialLenK;
        if (std::abs(v.lenCents - lc) < 0.01f)
            v.lenCents = lc;
    }
    // out of sight (a corner of the plan in between): the direct sound comes around the corner,
    // quieter and duller. 'dpos' is only the direct sound; the reflections and the room send keep
    // the pipe where it is.
    Pos dpos = pos;
    float hidDb = 0.0f, hidFc = 20000.0f;
    // a body in the room (room motion) is where it is - not placed by a ray from the listener's
    // usual place
    v.hasBody = isRoomMotion(p.motionMode[(size_t)v.stop]) && bodyAt(v.stop, v.key, v.bodyPos);
    if (v.fresh)
        v.mocc.valid = false;
    if (aroundCorner(p.stopRoom[(size_t)v.stop], homePos, dpos, hidDb, hidFc,
                     v.hasBody ? &v.bodyPos : nullptr, &v.mocc) == 2)
        ++occCount;
    profMark(9);
    {
        // Ventil: how far this pipe's pallet is open (a slow key opens it slowly; or the finger's
        // pressure / slide)
        const auto &lv = p.live;
        if (lv.pallet <= 0.0001f)
            v.palOpen = v.palWind = 1.0f;
        else if (lv.palletSrc == 0)
        {
            const float slow = (1.0f - v.velMod) * (1.0f - v.velMod);
            const float T = 1.6f * lv.pallet * slow;
            if (v.fresh)
                v.palOpen =
                    T > 0.002f ? juce::jmax(0.1f, 1.0f - lv.pallet * (0.3f + 0.7f * slow)) : 1.0f;
            else if (v.palOpen < 1.0f)
                v.palOpen =
                    T > 0.002f ? juce::jmin(1.0f, v.palOpen + (float)blockSeconds / T) : 1.0f;
            v.palWind = v.palOpen;
        }
        else
        {
            const float t = lv.palletSrc == 1 ? voicePressure(v) : voiceSlide(v);
            if (v.fresh)
                v.palOpen = t;
            else
                v.palOpen += (1.0f - std::exp(-(float)blockSeconds / 0.012f)) * (t - v.palOpen);
            v.palWind = 1.0f - lv.pallet * (1.0f - v.palOpen);
        }
    }
    modAz[(size_t)v.stop][(size_t)v.key] = 180.0f * pm[tgtAz];
    modEl[(size_t)v.stop][(size_t)v.key] = 90.0f * pm[tgtEl];
    modDistMul[(size_t)v.stop][(size_t)v.key] =
        std::exp2(3.0f * juce::jlimit(-2.0f, 2.0f, pm[tgtDist]));
    v.modCents1 = 1200.0f * juce::jlimit(-4.0f, 4.0f, pm[tgtPitch]);
    v.pmWind = pm[tgtWind];
    v.pmDeg = pm[tgtDegrade];
    v.pmWob = pm[tgtWobble];
    v.pmCrk = pm[tgtCrackle];
    {
        // the frequency this pipe sounds at now: as recorded (measured) or as the tuning means it
        // (nominal)
        const Pipe &pp = *v.pipe;
        double ratio = 1.0;
        if (pp.substituted && pp.substituteFrom >= 0)
            ratio = meantoneHz(pp.midi) /
                    meantoneHz(kPhysKeys[(size_t)juce::jlimit(0, kNumKeys - 1, pp.substituteFrom)]);
        const double rec = (pp.measuredHz > 0.0 ? pp.measuredHz : pp.nominalHz) * ratio,
                     ideal = (pp.nominalHz > 0.0 ? pp.nominalHz : pp.measuredHz) * ratio;
        const float cents = v.lenCents + (v.noteId >= 0 ? v.noteCents : p.tuning.cents[(size_t)v.key]) +
                            p.pitchCents + p.detune * v.detuneRnd + v.modCents1 +
                            (mpeNow ? v.bendSm : 0.0f) + v.pureC;
        v.hzNow = (float)((p.beatIdeal ? ideal : rec) * std::exp2(cents / 1200.0) * (double)v.dop);
    }
    v.pmx = pm;
    v.pmNew = false;
    for (int t = tgtAttack; t < kNumPipeTargets; ++t)
        if (pm[(size_t)t] != 0.0f)
        {
            v.pmNew = true;
            break;
        }
    if (v.fresh && v.pmNew)
    {
        // what is decided when the note starts: how often the chiff repeats, a hard start, the
        // first glitch
        const auto ps = pipeSndFor(p.pipeSnd[(size_t)v.stop], v);
        if (pm[tgtAttRep] != 0.0f)
            v.chLeft = ps.attRep > 0.0001f ? 1 + (int)std::round(ps.attRep * 5.0f) : 0;
        const auto gl = glitchFor(p.glitch, v);
        if (gl.on() && !p.glitch.on())
        {
            v.glTimer = (int)(rng.uni() * 0.3 * sr) + 1;
            v.clipTimer = (int)(rng.uni() * 0.3 * sr) + 1;
        }
        if (p.glitch.zero <= 0.0001f && gl.zero > 0.0001f && v.pos == 0.0 && rng.uni() < gl.zero)
            v.pos = (double)rng.uni() * gl.zero * (v.pipe->onset + 0.03 * kSampleFileRate);
    }

    profMark(10);
    gainsFor(dpos, v.g1.data());
    const float sg = p.stopGain[(size_t)v.stop];
    const float lvl = anyMod ? dbToGain(juce::jlimit(-100.0f, 12.0f, 36.0f * pm[tgtLevel])) : 1.0f;
    v.hard = (Motion)p.motionMode[(size_t)v.stop] == Motion::Jumps && p.jumpHard[(size_t)v.stop];
    v.pmAz = anyMod ? 180.0f * pm[tgtAz] : 0.0f;
    v.pmEl = anyMod ? 90.0f * pm[tgtEl] : 0.0f;
    v.pmDist = anyMod ? std::exp2(3.0f * juce::jlimit(-2.0f, 2.0f, pm[tgtDist])) : 1.0f;
    v.gainLin = sg * lvl;
    float g =
        sg * lvl * distanceGain(dpos.dist, p.distAmount) * (hidDb > 0.0f ? dbToGain(-hidDb) : 1.0f);
    {
        // Fluestern: a pipe within arm's reach of the head is also heard at the two ears
        // themselves - each ear by its own distance (a strong difference, as nothing far away
        // has), the far one later and in the head's shadow, and warmer the nearer it is
        v.near.q0 = v.near.q1;
        Voice::NearQ q;
        const float w = p.live.whisper;
        if (w > 0.0001f && nearHeard.load())
        {
            const float a = juce::degreesToRadians(homePos.az), e = juce::degreesToRadians(homePos.el);
            float x = -std::sin(a) * std::cos(e) * homePos.dist - walkX,
                  y = std::cos(a) * std::cos(e) * homePos.dist - walkY,
                  z = std::sin(e) * homePos.dist - walkZ;
            if (v.hasBody)
            {
                // a pipe that flies or is carried is where it is in the Saal (directions never
                // come nearer than half a metre; this one may be at the ear)
                const Vec3f L = erListener[(size_t)juce::jlimit(0, kNumRooms - 1, p.stopRoom[(size_t)v.stop])];
                x = v.bodyPos.x - L.x;
                y = v.bodyPos.y - L.y;
                z = v.bodyPos.z - L.z;
            }
            const float d = std::sqrt(x * x + y * y + z * z);
            constexpr float kReach = 1.4f, kEar = 0.0875f;
            if (d < kReach)
            {
                float t = juce::jlimit(0.0f, 1.0f, (kReach - d) / (kReach - 0.2f));
                t = t * t * (3.0f - 2.0f * t);
                const float yaw = juce::degreesToRadians(headYaw.load());
                const float side = x * std::cos(yaw) + y * std::sin(yaw),
                            front = -x * std::sin(yaw) + y * std::cos(yaw);
                const float dR = std::sqrt((side - kEar) * (side - kEar) + front * front + z * z),
                            dL = std::sqrt((side + kEar) * (side + kEar) + front * front + z * z);
                const float base = 0.9f * w * t * sg * lvl * (hidDb > 0.0f ? dbToGain(-hidDb) : 1.0f);
                const float nearest = juce::jmin(dL, dR), perM = (float)sr / kSpeedOfSoundRoom;
                // (the far ear loses more than its distance alone would take: so close, the head
                // shadows the low notes too)
                auto ear = [&](float de)
                {
                    return base * juce::jmin(12.0f, 1.0f / juce::jmax(0.05f, de)) *
                           std::pow(juce::jmax(0.03f, nearest) / juce::jmax(0.03f, de), 0.7f);
                };
                q.gL = ear(dL);
                q.gR = ear(dR);
                q.dL = juce::jmin(100.0f, (dL - nearest) * perM);
                q.dR = juce::jmin(100.0f, (dR - nearest) * perM);
                auto shadow = [&](float de)
                {
                    const float fc = juce::jlimit(1500.0f, 18000.0f, 18000.0f / (1.0f + 4.0f * (de / juce::jmax(0.03f, nearest) - 1.0f)));
                    return 1.0f - std::exp(-juce::MathConstants<float>::twoPi * juce::jmin(0.45f * (float)sr, fc) / (float)sr);
                };
                q.aL = dL > nearest ? shadow(dL) : 1.0f;
                q.aR = dR > nearest ? shadow(dR) : 1.0f;
                q.bass = t;
                // (it leaves the room's own picture as it comes into the ears: both at once would
                // colour each other)
                g *= 1.0f - 0.85f * w * t;
            }
        }
        if (q.gL + q.gR <= 0.0f)
        {
            // (fading out: the delays and filters stay as they were)
            q.dL = v.near.q0.dL;
            q.dR = v.near.q0.dR;
            q.aL = v.near.q0.aL;
            q.aR = v.near.q0.aR;
            q.bass = v.near.q0.bass;
        }
        v.near.q1 = q;
        if (v.fresh)
            v.near.q0 = q;
    }
    // grain cloud: where the pipe is (the grains scatter around it); the pipe itself fades as it
    // dissolves (equal power)
    v.spPos = dpos;
    v.spK = g;
    {
        const auto cl = cloudFor(p.cloud[(size_t)v.stop], v);
        v.cloudDry =
            cl.on()
                ? std::cos(0.5f * juce::MathConstants<float>::pi * juce::jlimit(0.0f, 1.0f, cl.mix))
                : 1.0f;
        g *= v.cloudDry;
    }
    for (int c = 0; c < kMaxAmbiChannels; ++c)
        v.g1[(size_t)c] *= g;
    if (p.fx.partSpread > 0.5f)
    {
        Pos ph = dpos;
        ph.az = wrapAz(dpos.az + p.fx.partSpread * v.partSide + (float)partRot);
        gainsFor(ph, v.gh1.data());
        for (int c = 0; c < kMaxAmbiChannels; ++c)
            v.gh1[(size_t)c] *= g;
    }
    else
        v.gh1 = v.g1;
    v.crkEnv = v.held ? 1.0f : v.crkEnv * spatialCrk;
    v.send1 =
        sg * lvl *
        distanceGain(pos.dist, p.distAmount * 0.35f); // the room fades much less with distance
    v.erBase = sg * lvl;
    v.erPos = homePos; // (from the usual place: the reflections place it from there)
    v.sendAz1 = p.roomFollow ? pos.az : p.roomBaseAz;
    if (anyMod && pm[tgtSend] != 0.0f)
        v.send1 *= dbToGain(juce::jlimit(-100.0f, 24.0f, 24.0f * pm[tgtSend]));

    // jumps: a ~4 ms glide so the teleport is sudden but does not click
    if (!v.fresh && !v.hard && (Motion)p.motionMode[(size_t)v.stop] == Motion::Jumps &&
        blockSeconds > 0.0)
    {
        const float k = 1.0f - (float)std::exp(-blockSeconds / 0.004);
        for (int c = 0; c < kMaxAmbiChannels; ++c)
            v.g1[(size_t)c] = v.g0[(size_t)c] + k * (v.g1[(size_t)c] - v.g0[(size_t)c]);
    }

    // air: far pipes lose their highs
    float fc = juce::jlimit(1200.0f, 20000.0f, 20000.0f / (1.0f + p.air * pos.dist / 5.0f));
    if (anyMod && pm[tgtBright] != 0.0f)
        fc = juce::jlimit(40.0f, 20000.0f, fc * std::exp2(6.0f * pm[tgtBright]));
    v.airA = fc >= 19999.0f ? 1.0f
                            : 1.0f - std::exp(-juce::MathConstants<float>::twoPi * fc / (float)sr);
    // around a corner: the direct sound alone gets duller (the reflections reach the listener's
    // side as they are)
    v.hidA = hidFc >= 19999.0f ? 1.0f
                               : 1.0f - std::exp(-juce::MathConstants<float>::twoPi *
                                                 juce::jmax(200.0f, hidFc) / (float)sr);

    // Doppler from the radial speed (not for teleporting pipes)
    if (v.prevDist < 0.0f || (Motion)p.motionMode[(size_t)v.stop] == Motion::Jumps ||
        p.doppler <= 0.0f || blockSeconds <= 0.0)
        v.dopTarget = 1.0f;
    else
    {
        const float vr =
            juce::jlimit(-120.0f, 120.0f, (float)((pos.dist - v.prevDist) / blockSeconds));
        v.dopTarget = juce::jlimit(0.5f, 2.0f, kSpeedOfSound / (kSpeedOfSound + p.doppler * vr));
    }
    v.prevDist = pos.dist;
    profMark(11);
}

// ---------------------------------------------------------------------------
float OrganEngine::readSample(const float *d, int len, double pos) const noexcept
{
    // 4-point cubic Hermite
    const int i = (int)pos;
    const float f = (float)(pos - i);
    float xm1, x0, x1, x2;
    if (i >= 1 && i + 2 < len)
    {
        xm1 = d[i - 1];
        x0 = d[i];
        x1 = d[i + 1];
        x2 = d[i + 2];
    } // (almost always)
    else
    {
        auto at = [d, len](int j) { return (j >= 0 && j < len) ? d[j] : 0.0f; };
        xm1 = at(i - 1);
        x0 = at(i);
        x1 = at(i + 1);
        x2 = at(i + 2);
    }
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}

/** freeze: overlapping grains taken from the sustain of the pipe, slightly detuned */
float OrganEngine::grainSample(Voice &v, double rate, float smear) noexcept
{
    const Pipe &pipe = *v.pipe;
    if (--v.grainTimer <= 0)
    {
        int regionStart = pipe.hasLoop() ? pipe.loopStart
                                         : juce::jmin(pipe.attackEnd + (int)(0.1 * kSampleFileRate),
                                                      pipe.release);
        int regionEnd = pipe.hasLoop() ? pipe.loopEnd : pipe.release;
        if (regionEnd - regionStart < 4096)
        {
            regionStart = juce::jmax(0, pipe.attackEnd);
            regionEnd = juce::jmax(regionStart + 16, pipe.release);
        }
        const int glen = (int)((0.06 + 0.5 * smear) * sr);
        for (auto &g : v.grains)
            if (!g.on)
            {
                const double span =
                    juce::jmax(1.0, (double)(regionEnd - regionStart) - glen * rate * 1.1);
                g.pos = regionStart + v.grainRng.uni() * span;
                g.len = juce::jmax(64, glen);
                g.age = 0;
                g.rateMul = std::exp2(smear * 25.0f * v.grainRng.bi() / 1200.0f);
                g.on = true;
                break;
            }
        v.grainTimer = juce::jmax(16, (int)((float)glen / 3.0f * (0.7f + 0.6f * v.grainRng.uni())));
    }
    float s = 0.0f;
    for (auto &g : v.grains)
    {
        if (!g.on)
            continue;
        const float ph = (float)g.age / (float)g.len;
        const float w = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * ph);
        s += w * readSample(v.data, v.length, g.pos);
        g.pos += rate * g.rateMul;
        if (++g.age >= g.len)
            g.on = false;
    }
    return s * (1.0f / 1.5f);
}

void OrganEngine::startVoice(int stop, int key, int previewSamples, int noteId)
{
    if (library == nullptr)
        return;
    const int src = noteId >= 0 ? key : juce::jlimit(0, kNumKeys - 1, keySource[(size_t)key]);
    const Pipe &pipe = library->pipes[(size_t)stop][(size_t)src];
    if (!pipe.valid || pipe.sample == nullptr)
        return;

    SamplePitchRequest pitchRequest;
    pitchRequest.sourceSampleRate = pipe.sample->sampleRate;
    pitchRequest.outputSampleRate = sr;
    pitchRequest.nativePitchRatio = pipe.nativePitchRatio;

    const auto nativeRate = samplePlaybackRate(pitchRequest, SampleTuningMode::native);

    if (!nativeRate.has_value())
        return;

    if (previewSamples < 0 && muted[(size_t)stop][(size_t)key].load())
        return;

    Voice *v = nullptr;
    for (auto &c : voices)
        if (!c.active)
        {
            v = &c;
            break;
        }
    if (v == nullptr)
    {
        // steal the oldest releasing voice, else the oldest one
        Voice *oldest = nullptr;
        for (auto &c : voices)
            if (!c.held && (oldest == nullptr || c.age < oldest->age))
                oldest = &c;
        if (oldest == nullptr)
            for (auto &c : voices)
                if (oldest == nullptr || c.age < oldest->age)
                    oldest = &c;
        v = oldest;
    }

    if (v->active)
        releaseSlot(v->erSlot);
    v->active = true;
    v->erSlot = roomOnNow ? allocSlot((int)(v - voices.data()), -1) : -1;
    if (previewSamples < 0)
    {
        lastStruckStop = stop;
        lastStruckKey = key;
    }
    v->stop = stop;
    v->key = key;
    v->sourceKey = src;
    v->pipe = &pipe;
    v->data = pipe.sample->data.data();
    v->length = pipe.sample->size();
    v->pos = 0.0;
    v->baseRate = *nativeRate;
    v->looping = pipe.hasLoop();
    if (v->looping)
    {
        const int loopLen = pipe.loopEnd - pipe.loopStart;
        v->loopXf = juce::jlimit(16, juce::jmax(16, pipe.loopStart), juce::jmin(1440, loopLen / 3));
    }
    v->held = true;
    v->relXfade = false;
    v->killing = false;
    v->killGain = 1.0f;
    v->killStep = (float)(1.0 / (0.005 * sr));
    v->previewRemain = previewSamples;
    v->near = {};
    v->lenCents = -1200.0f * std::log2(juce::jlimit(0.2f, 4.0f, pipeLen[(size_t)stop][(size_t)key].load()));
    v->age = ++ageCounter;
    v->cloudRing = -1;
    v->cloudAcc = 0.0;
    v->cloudDry = 1.0f;
    v->pmx.fill(0.0f);
    v->pmNew = false;
    v->hiGSm = v->airInvSm = -1.0f;
    v->hzNow = 0.0f;
    v->beatT = v->beatHz = v->beatNorm = 0.0f;
    {
        // Ansprache: the chiff repeated as a short burst (a second reader over the attack)
        const auto &ps = pipeSndNow[(size_t)stop];
        v->attLp = 0.0f;
        v->chLeft = ps.attRep > 0.0001f ? 1 + (int)std::round(ps.attRep * 5.0f) : 0;
        v->chLen =
            juce::jmax(64, juce::jmin(pipe.attackEnd - pipe.onset, (int)(0.09 * kSampleFileRate)));
        v->chWait = (int)((double)(pipe.onset + v->chLen * 0.7) / juce::jmax(1.0e-6, v->baseRate));
        v->chAge = -1;
        v->chCount = 0;
        v->chPos = 0.0;
        // Abriss, upper partials, glitches
        v->puffPending = false;
        v->puffEnv = 0.0f;
        v->py1 = v->py2 = 0.0f;
        v->pLp = 0.0f;
        v->hiS1 = v->hiS2 = 0.0f;
        v->hiEf = v->hiEs = 0.0f;
        v->stCount = 0;
        v->stAcc = 0;
        v->clipLeft = 0;
        v->splLeft = 0;
        v->glTimer = v->clipTimer = 1;
        if (glitchNow.on())
        {
            v->glTimer = (int)(rng.uni() * 0.3 * sr) + 1;
            v->clipTimer = (int)(rng.uni() * 0.3 * sr) + 1;
        }
        // no care for zero crossings: the note may start anywhere in its first moments (a hard
        // edge)
        if (glitchNow.zero > 0.0001f && rng.uni() < glitchNow.zero)
            v->pos = (double)rng.uni() * glitchNow.zero * (pipe.onset + 0.03 * kSampleFileRate);
    }
    v->fresh = true;
    v->env = 0.0f;
    v->held1 = 0.0f;
    v->windSm = -1.0f;
    v->lp = 0.0f;
    v->lpA = 1.0f;
    v->lpAir = 0.0f;
    v->airA = 1.0f;
    v->hidLp = 0.0f;
    v->hidA = 1.0f;
    v->hidAc = 1.0f;
    v->chaos.reset((uint32_t)(stop * 7919 + key * 104729) ^ (ageCounter * 2654435761u),
                   (uint32_t)(stop * 131 + key + 1),
                   (float)(pipe.measuredHz > 0.0 ? pipe.measuredHz : pipe.nominalHz), (float)sr);
    v->chAir = 0.0f;
    v->prevDist = -1.0f;
    v->dop = 1.0f;
    v->dopTarget = 1.0f;
    XorShift dr((uint32_t)(stop * 7919 + key * 104729 + 3));
    dr.next();
    v->detuneRnd = dr.bi();
    v->frozen = false;
    v->freezeMix = 0.0f;
    v->grainTimer = 0;
    for (auto &g : v->grains)
        g.on = false;
    for (auto &e : v->menv)
    {
        e.reset();
        e.trigger();
    }
    v->keyPos = (float)key / (float)(kNumKeys - 1) * 2.0f - 1.0f;
    v->randMod = lastRandom;
    v->noteId = noteId;
    v->noteCents = noteId >= 0 ? centsFor(noteId, key) : 0.0f;
    v->bendSm = voiceBendTarget(*v);
    v->velMod = keyVelocity[(
        size_t)key]; // (a stop pulled later takes the velocity the key was struck with)
    v->modCents0 = v->modCents1 = 0.0f;
    v->palOpen = v->palWind = 1.0f;
    v->wearNow = wear[(size_t)stop][(size_t)key].load();
    v->wearSign = hash01((uint32_t)(stop * 613 + key * 89 + 11)) < 0.6f
                      ? -1.0f
                      : 1.0f; // most worn pipes go flat
    v->pureT = v->pureC = 0.0f;
    v->crkAmp = 0.0f;
    v->crkEnv = 1.0f;
    v->crkCount = 1 + (int)(rng.uni() * 0.2f * (float)sr);
    v->partLp = 0.0f;
    v->partSide = (hash01((uint32_t)(stop * 977 + key * 31 + 5)) < 0.5f ? -1.0f : 1.0f) *
                  (0.6f + 0.4f * hash01((uint32_t)(stop * 13 + key * 101 + 9)));
}

void OrganEngine::releaseVoice(Voice &v)
{
    if (!v.held)
        return;
    v.held = false;
    if (pipeSndFor(pipeSndNow[(size_t)v.stop], v).puff > 0.0001f)
        v.puffPending = true; // Abriss: the pallet closes
    const Pipe &p = *v.pipe;
    const int relLenFile = (int)(0.04 * kSampleFileRate);
    const double target = (double)juce::jmax(0, p.release - relLenFile);
    if (v.pos < target)
    {
        // crossfade from wherever we are (usually inside the loop) to just before the
        // recorded key release, then let the real release tail play
        v.relXfade = true;
        v.relPos = target;
        v.relCount = 0;
        v.relLen = juce::jmax(
            1, (int)(0.04 * sr *
                     (1.0 - 0.995 * glitchFor(glitchNow, v).zero))); // glitch: a hard cut instead
    }
    else
    {
        v.looping = false;
    }
}

void OrganEngine::freezeVoice(Voice &v)
{
    if (!v.held)
        return;
    v.held = false; // no longer belongs to a key
    v.frozen = true;
    v.freezeMix = 0.0f;
    v.grainTimer = 0;
}

void OrganEngine::releaseStopKey(int stop, int key, int noteId)
{
    for (auto &v : voices)
        if (v.active && v.held && v.stop == stop && v.key == key && v.previewRemain < 0 &&
            (noteId < 0 || v.noteId == noteId))
            releaseVoice(v);
}

// ---------------------------------------------------------------------------
// free tuning: every MIDI note its own pitch (Scala file, MTS-ESP)
// ---------------------------------------------------------------------------
float OrganEngine::centsFor(int noteId, int key) const noexcept
{
    if (notesNow == nullptr || noteId < 0 || noteId >= NoteTable::kSize)
        return 0.0f;
    const float hz = notesNow->hz[(size_t)tableIndex(noteId)];
    if (!(hz > 1.0f))
        return 0.0f;
    return 1200.0f * std::log2(hz / slotHz[(size_t)key]);
}

void OrganEngine::noteOnFree(const juce::MidiMessage &m, const EngineParams &p)
{
    const int id = (juce::jlimit(1, 16, m.getChannel()) - 1) * 128 + m.getNoteNumber();
    if (notesNow == nullptr || !notesNow->mapped[(size_t)tableIndex(id)])
        return;
    const float hz = notesNow->hz[(size_t)tableIndex(id)];
    if (!(hz > 1.0f))
        return;
    const int key = keyIndexForMidi(m.getNoteNumber());
    int k = 0;
    if (notesNow == &keyboardTable && key >= 0)
        k = juce::jlimit(0, kNumKeys - 1, p.tuning.source[static_cast<std::size_t>(key)]);
    else
    {
        // For a chromatic gap or external tuning, use the nearest pipe.
        float best = 1.0e9f;
        for (int i = 0; i < kNumKeys; ++i)
        {
            const float distance = std::abs(std::log2(hz / slotHz[static_cast<std::size_t>(i)]));
            if (distance < best)
            {
                best = distance;
                k = i;
            }
        }
        const int midi = m.getNoteNumber();
        const bool extended =
            p.extendedRange && midi >= kExtendedLowestMidiNote && midi <= kExtendedHighestMidiNote;
        if (best > 2.0f && !extended)
            return;
    }
    auto &fn = freeNotes[(size_t)id];
    if (fn.held)
        keyUp(fn.key, p, id); // struck again without a note-off
    fn.held = true;
    fn.sustained = false;
    fn.key = k;
    ++keyNoteCount[(size_t)k];
    keyDown(k, m.getFloatVelocity(), p, id);
}

void OrganEngine::noteOffFree(const juce::MidiMessage &m, const EngineParams &p)
{
    const int id = (juce::jlimit(1, 16, m.getChannel()) - 1) * 128 + m.getNoteNumber();
    auto &fn = freeNotes[(size_t)id];
    if (!fn.held)
        return;
    if (sustainPedal)
    {
        fn.sustained = true;
        return;
    }
    keyUp(fn.key, p, id);
}

void OrganEngine::releaseAllKeys(const EngineParams &p)
{
    for (int id = 0; id < NoteTable::kSize; ++id)
        if (freeNotes[(size_t)id].held)
            keyUp(freeNotes[(size_t)id].key, p, id);
    for (int k = 0; k < kNumKeys; ++k)
    {
        keySustained[(size_t)k] = false;
        if (keyHeld[(size_t)k])
            keyUp(k, p);
        keyNoteCount[(size_t)k] = 0;
    }
}

OrganEngine::NoiseVoice *OrganEngine::playNoise(const std::vector<NoiseSample> &pool, float gain,
                                                const float *gains, int &lastIndex, float send,
                                                float tone)
{
    if (pool.empty() || gain <= 0.0001f)
        return nullptr;
    int idx = (int)(rng.uni() * (float)pool.size());
    if (pool.size() > 1 && idx == lastIndex)
        idx = (idx + 1) % (int)pool.size();
    idx = juce::jlimit(0, (int)pool.size() - 1, idx);
    lastIndex = idx;
    const auto &s = pool[(size_t)idx].sample;
    NoiseVoice *nv = nullptr;
    for (auto &n : noise)
        if (!n.active)
        {
            nv = &n;
            break;
        }
    if (nv == nullptr)
        nv = &noise[(size_t)(rng.next() % noise.size())];
    if (nv->active)
        releaseSlot(nv->erSlot);
    nv->active = true;
    nv->where = {consoleAzNow, consoleElNow, 2.0f}; // reflections: from the console
    nv->erSlot = roomOnNow ? allocSlot(-1, (int)(nv - noise.data())) : -1;
    nv->data = s->data.data();
    nv->length = s->size();
    nv->pos = 0.0;
    nv->rate = s->sampleRate / sr * (0.97 + 0.06 * rng.uni()); // tiny variation per click
    nv->gain = gain * (0.85f + 0.3f * rng.uni());
    nv->send = send;
    nv->lp = 0.0f;
    nv->lpA = juce::jlimit(0.02f, 1.0f, tone);
    nv->azVel = 0.0f;
    nv->resN = 0;
    nv->resTail = 0;
    std::copy(gains, gains + kMaxAmbiChannels, nv->g.begin());
    nv->sendW = noiseRoomW;
    return nv;
}

/** resonated clicks: the click rings the harmonics of the pressed key (in the current tuning) */
void OrganEngine::setupResonators(NoiseVoice &nv, int key, float velocity,
                                  const EngineParams &p) noexcept
{
    if (p.resClick <= 0.0001f || library == nullptr)
        return;
    // the key's pitch at 8': the Praestant pipe of this key (or the nominal pitch), moved by the
    // tuning
    const auto &pp = library->pipes[0][(size_t)juce::jlimit(0, kNumKeys - 1, key)];
    double hz = pp.valid ? (pp.measuredHz > 0.0 ? pp.measuredHz : pp.nominalHz) : 0.0;
    if (hz <= 0.0)
        hz = kA4 *
             std::pow(2.0, ((double)kPhysKeys[(size_t)juce::jlimit(0, kNumKeys - 1, key)] - 69.0) /
                               12.0);
    hz *= std::exp2((p.tuning.cents[(size_t)juce::jlimit(0, kNumKeys - 1, key)] + p.pitchCents) /
                    1200.0);
    const float decay = juce::jlimit(0.02f, 4.0f, p.resDecay);
    int n = 0;
    for (int h = 1; h <= 6 && n < 6; ++h)
    {
        const double f = hz * h;
        if (f > 0.45 * sr)
            break;
        const double tau = decay / std::sqrt((double)h); // the upper harmonics die sooner
        const double r = std::exp(-1.0 / (tau * sr));
        const double w = juce::MathConstants<double>::twoPi * f / sr;
        nv.rb1[(size_t)n] = (float)(2.0 * r * std::cos(w));
        nv.rb2[(size_t)n] = (float)(-r * r);
        // peak gain of a two-pole resonator ~ 1 / (1 - r): normalised, then tilted by brightness
        const float tilt =
            std::pow((float)h, -1.6f * (1.0f - juce::jlimit(0.0f, 1.0f, p.resBright)));
        nv.rg[(size_t)n] = (float)((1.0 - r) * 0.5) * tilt * 6.0f;
        nv.ry1[(size_t)n] = nv.ry2[(size_t)n] = 0.0f;
        ++n;
    }
    nv.resN = n;
    nv.resMix =
        juce::jlimit(0.0f, 1.0f, p.resClick) * (0.6f + 0.4f * juce::jlimit(0.0f, 1.0f, velocity));
    nv.resTail = (int)(decay * 6.0 * sr);
}

void OrganEngine::keyDown(int key, float velocity, const EngineParams &p, int noteId)
{
    keyHeld[(size_t)key] = true;
    keySustained[(size_t)key] = false;
    globalTrig = true;
    lastKeyPos = (float)key / (float)(kNumKeys - 1) * 2.0f - 1.0f;
    lastRandom = rng.bi();
    lastVelocity = juce::jlimit(0.0f, 1.0f, velocity);
    keyVelocity[(size_t)key] = lastVelocity;
    lastDownKey = key;

    // click: blend between the console position and the average position of the pipes
    std::array<float, kMaxAmbiChannels> console{}, pipes{}, tmp{};
    gainsFor({p.consoleAz, p.consoleEl}, console.data());
    int n = 0;
    for (int s = 0; s < kNumStops; ++s)
    {
        if (!p.stopOn[(size_t)s])
            continue;
        gainsFor(currentPosition(s, key, p), tmp.data());
        for (int c = 0; c < kMaxAmbiChannels; ++c)
            pipes[(size_t)c] += tmp[(size_t)c];
        ++n;
    }
    std::array<float, kMaxAmbiChannels> g{};
    for (int c = 0; c < kMaxAmbiChannels; ++c)
        g[(size_t)c] = n > 0 ? (1.0f - p.clickBlend) * console[(size_t)c] +
                                   p.clickBlend * pipes[(size_t)c] / (float)n
                             : console[(size_t)c];
    if (library != nullptr)
    {
        // velocity: a light touch gives a quieter and duller click
        const float v = juce::jlimit(0.0f, 1.0f, velocity);
        keyVelocity[(size_t)key] = v;
        if (auto *nv = playNoise(
                library->clicksDown.empty() ? library->clicksOther : library->clicksDown,
                p.clickLevel * clickCurve(v) * 1.6f, g.data(), lastDown, 0.5f, clickTone(v)))
            setupResonators(*nv, key, v, p);
    }

    // effects: own sounds, ticks running to the pipes
    if (p.fx.custLevel > 0.0001f && rng.uni() < p.fx.custDown)
        customSounds(key, velocity, p);
    trackerPath(key, velocity, p);
    const bool wait = p.fx.pathWait && p.fx.pathLevel > 0.0001f && p.fx.pathTime > 0.005f;

    // notes to single pipes: only one stop sounds this note (the others still let an old one on
    // this key go)
    const int only = p.noteSpread > 0 ? spreadStopFor(key, p) : -2;
    for (int s = 0; s < kNumStops; ++s)
        if (p.stopOn[(size_t)s])
        {
            releaseStopKey(s, key, noteId);
            if (only != -2 && s != only)
                continue;
            if (wait)
            {
                Scheduled e;
                e.voice = true;
                e.stop = s;
                e.key = key;
                e.noteId = noteId;
                e.delay = (int)(p.fx.pathTime * sr);
                schedule(e);
            }
            else
                startVoice(s, key, -1, noteId);
        }
}

int OrganEngine::spreadStopFor(int key, const EngineParams &p) noexcept
{
    // the pulled stops in the order of the console (left to right); a muted pipe on this key is
    // skipped
    std::array<int, kNumStops> list{};
    int n = 0;
    for (int slot = 0; slot < kNumStops; ++slot)
    {
        const int s = kDisplayOrder[(size_t)slot];
        if (p.stopOn[(size_t)s] && !muted[(size_t)s][(size_t)key].load())
            list[(size_t)n++] = s;
    }
    if (n == 0)
        return -1;
    int idx = 0;
    if (p.noteSpread == 1)
        idx = (int)(spreadCounter++ % (uint32_t)n); // in turn
    else if (p.noteSpread == 2)
    {
        // by position: the lowest of the held notes on the first stop, the next one up on the
        // second ...
        int rank = 0;
        for (int k = 0; k < key; ++k)
            rank += keyHeld[(size_t)k] ? 1 : 0;
        idx = rank % n;
    }
    else
    {
        idx = (int)(rng.next() % (uint32_t)n); // at random (not twice the same)
        if (n > 1 && list[(size_t)idx] == lastSpreadStop)
            idx = (idx + 1) % n;
    }
    lastSpreadStop = list[(size_t)idx];
    return list[(size_t)idx];
}

void OrganEngine::keyUp(int key, const EngineParams &p, int noteId)
{
    if (noteId >= 0)
    {
        // free tuning: this note lets go; the pipe stays "held" while other notes still sound on it
        auto &fn = freeNotes[(size_t)noteId];
        if (!fn.held)
            return;
        fn.held = false;
        fn.sustained = false;
        keyNoteCount[(size_t)key] = juce::jmax(0, keyNoteCount[(size_t)key] - 1);
        keyHeld[(size_t)key] = keyNoteCount[(size_t)key] > 0;
    }
    else
    {
        if (!keyHeld[(size_t)key])
            return;
        keyHeld[(size_t)key] = false;
    }
    std::array<float, kMaxAmbiChannels> console{};
    gainsFor({p.consoleAz, p.consoleEl}, console.data());
    if (library != nullptr)
    {
        const float v = keyVelocity[(size_t)key]; // the key comes up about as hard as it went down
        if (auto *nv = playNoise(
                library->clicksUp.empty() ? library->clicksOther : library->clicksUp,
                p.releaseClick * clickCurve(v) * 1.6f, console.data(), lastUp, 0.5f, clickTone(v)))
            setupResonators(*nv, key, v, p);
    }
    if (p.fx.custLevel > 0.0001f && rng.uni() < p.fx.custUp)
        customSounds(key, 0.8f * keyVelocity[(size_t)key], p);
    for (auto &v : voices)
        if (v.active && v.held && v.key == key && v.previewRemain < 0 &&
            (noteId < 0 || v.noteId == noteId))
        {
            if (p.freeze)
                freezeVoice(v); // freeze: the pipe keeps sounding as a grain cloud
            else
                releaseVoice(v);
        }
}

void OrganEngine::handleMidi(const juce::MidiMessage &m, const EngineParams &p)
{
    const int ch = juce::jlimit(1, 16, m.getChannel());
    if (m.isNoteOn())
    {
        notePress[(size_t)((ch - 1) * 128 + m.getNoteNumber())] =
            0.0f; // (a new note starts unpressed)
        if (notesNow != nullptr)
        {
            noteOnFree(m, p);
            return;
        }
        const int k = keyIndexForMidi(m.getNoteNumber());
        if (k >= 0)
            keyDown(k, m.getFloatVelocity(), p);
    }
    else if (m.isNoteOff())
    {
        if (notesNow != nullptr)
        {
            noteOffFree(m, p);
            return;
        }
        const int k = keyIndexForMidi(m.getNoteNumber());
        if (k < 0)
            return;
        if (sustainPedal)
        {
            keySustained[(size_t)k] = true;
            return;
        }
        keyUp(k, p);
    }
    else if (m.isController() && m.getControllerNumber() == 1)
        modWheel = (float)m.getControllerValue() / 127.0f;
    else if (m.isController() && m.getControllerNumber() == 74)
    {
        chSlide[(size_t)(ch - 1)] = lastSlide = (float)m.getControllerValue() / 127.0f;
    }
    else if (m.isPitchWheel())
    {
        const float b = juce::jlimit(-1.0f, 1.0f, (float)(m.getPitchWheelValue() - 8192) / 8191.0f);
        chBend[(size_t)(ch - 1)] = b;
        if (!mpeNow || ch == 1)
            pitchWheel = b; // MPE: note channels bend their own notes, channel 1 is the wheel
    }
    else if (m.isChannelPressure())
    {
        chPress[(size_t)(ch - 1)] = lastPress = lastChanPress =
            (float)m.getChannelPressureValue() / 127.0f;
    }
    else if (m.isAftertouch())
    {
        const float a = (float)m.getAfterTouchValue() / 127.0f;
        notePress[(size_t)((ch - 1) * 128 + m.getNoteNumber())] = lastPress = a;
        const int k = keyIndexForMidi(m.getNoteNumber());
        if (k >= 0)
            keyPress[(size_t)k] = a;
    }
    else if (m.isSustainPedalOn())
        sustainPedal = true;
    else if (m.isSustainPedalOff())
    {
        sustainPedal = false;
        for (int k = 0; k < kNumKeys; ++k)
            if (keySustained[(size_t)k])
            {
                keySustained[(size_t)k] = false;
                keyUp(k, p);
            }
        for (int id = 0; id < NoteTable::kSize; ++id)
            if (freeNotes[(size_t)id].held && freeNotes[(size_t)id].sustained)
                keyUp(freeNotes[(size_t)id].key, p, id);
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        for (int id = 0; id < NoteTable::kSize; ++id)
            if (freeNotes[(size_t)id].held)
                keyUp(freeNotes[(size_t)id].key, p, id);
        for (int k = 0; k < kNumKeys; ++k)
            if (keyHeld[(size_t)k])
                keyUp(k, p);
        if (m.isAllSoundOff())
            for (auto &v : voices)
                if (v.active)
                {
                    v.killing = true;
                }
    }
}

// ---------------------------------------------------------------------------
void OrganEngine::renderRange(juce::AudioBuffer<float> &out, int start, int end, int blockLen,
                              const EngineParams &p)
{
    const int n = end - start;
    if (n <= 0)
        return;
    // (Verfall / Windschwankung per pipe: see the voice loop - they can be modulated pipe by pipe)
    // global wind: above ~35 % the wind supply starts to shake – shared by all pipes (same wind
    // chest)
    const float turbLin = juce::jlimit(0.0f, 1.0f, (p.wind - 0.35f) / 0.65f);
    const float turb = turbLin * turbLin;
    const float turbCents = 190.0f * turb, turbAmp = 0.75f * turb;
    const double dt = 1.0 / sr;
    const float twoPiOverSr = (float)(juce::MathConstants<double>::twoPi / sr);
    const float freezeStep = (float)(1.0 / (0.15 * sr));
    const float tremCents = p.tremOn ? 16.0f * p.tremDepth : 0.0f;
    const float tremAmp = p.tremOn ? 0.32f * p.tremDepth : 0.0f;

    // shared curves for this range (one value per 16 samples): wind gusts and tremulant
    const int steps = n / 16 + 2;
    if ((int)gust.size() < steps)
    {
        gust.assign((size_t)steps, 0.0f);
        trem.assign((size_t)steps, 0.0f);
    }
    for (int j = 0; j < steps; ++j)
    {
        gust[(size_t)j] =
            turb > 0.0f ? smoothNoise(gustPhase + j * 16.0 * dt * (0.5 + 6.0 * turb), 4242u) : 0.0f;
        // a tremulant is a periodic interruption of the wind: slightly asymmetric wave
        const double tp =
            juce::MathConstants<double>::twoPi * (tremPhase + j * 16.0 * dt * p.tremRate);
        trem[(size_t)j] =
            p.tremOn ? (float)(std::sin(tp) + 0.25 * std::sin(2.0 * tp + 0.7)) / 1.2f : 0.0f;
    }
    gustPhase += n * dt * (0.5 + 6.0 * turb);
    tremPhase = std::fmod(tremPhase + n * dt * p.tremRate, 1.0);

    const float inv = 1.0f / (float)blockLen;
    float *const *outs = out.getArrayOfWritePointers();

    for (auto &v : voices)
    {
        if (!v.active)
            continue;
        const float *d = v.data;
        const int len = v.length;
        const Pipe &pipe = *v.pipe;
        const int loopLen = pipe.loopEnd - pipe.loopStart;
        float *buf = scratch.data();
        bool finished = false;
        // (the shared bellows and this pipe's pallet let only part of the stop's wind through)
        const float windTarget = juce::jlimit(0.0f, 1.0f, p.stopWind[(size_t)v.stop] + v.pmWind) *
                                 juce::jmin(1.0f, bellowsP) * v.palWind;
        // Verschleiss: a worn pipe crumbles, wavers and goes out of tune
        const float wr =
            p.live.wear > 0.0001f ? juce::jlimit(0.0f, 1.0f, v.wearNow * p.live.wear) : 0.0f;
        // Verfall and Windschwankung of this pipe (the global settings, plus its own modulation)
        DegradeParams degV = p.deg;
        degV.amount = juce::jlimit(0.0f, 1.0f, p.deg.amount + v.pmDeg);
        degV.wobble = juce::jlimit(0.0f, 1.0f, p.deg.wobble + v.pmWob);
        if (v.pmNew && (v.pmx[tgtDegGrain] != 0.0f || v.pmx[tgtDegBits] != 0.0f ||
                        v.pmx[tgtDegRate] != 0.0f || v.pmx[tgtDegDrops] != 0.0f))
        {
            degV.grain = juce::jlimit(0.0f, 1.0f, p.deg.grain + v.pmx[tgtDegGrain]);
            degV.bits = juce::jlimit(3.0f, 16.0f, p.deg.bits + 13.0f * v.pmx[tgtDegBits]);
            degV.rateHz = juce::jlimit(
                600.0f, 48000.0f,
                p.deg.rateHz * std::exp2(6.0f * juce::jlimit(-2.0f, 2.0f, v.pmx[tgtDegRate])));
            degV.dropouts = juce::jlimit(0.0f, 1.0f, p.deg.dropouts + v.pmx[tgtDegDrops]);
            degV.prepareBlock(sr);
        }
        const float wobAmt = juce::jlimit(0.0f, 1.0f, degV.amount * degV.wobble + 0.45f * wr * wr);
        if (wr > 0.0f)
            degV.amount = juce::jlimit(0.0f, 1.0f, (p.deg.perPipe ? degV.amount : 0.0f) + wr);
        const bool perPipeDeg = (p.deg.perPipe || wr > 0.0f) && degV.amount > 0.0001f;
        // "Windschwankung" gets disproportionately wilder towards 100 %
        const float wobCents = 30.0f * wobAmt + 320.0f * wobAmt * wobAmt * wobAmt * wobAmt;
        const float wobLevel = 0.3f * wobAmt + 0.6f * wobAmt * wobAmt * wobAmt * wobAmt;
        const float wobSpeed = 1.0f + 5.0f * wobAmt * wobAmt * wobAmt;
        double tuneCents = v.lenCents + (v.noteId >= 0 ? v.noteCents : p.tuning.cents[(size_t)v.key]) +
                           p.pitchCents + p.detune * v.detuneRnd + v.pureC +
                           38.0f * wr * wr * v.wearSign * (0.4f + 0.6f * std::abs(v.detuneRnd));
        if (v.noteId < 0)
        {
            // The tuning describes the target relative to its preferred pipe.
            // A held voice may still be playing a pipe from the previous tuning.
            const int targetSource = juce::jlimit(0, kNumKeys - 1, keySource[(size_t)v.key]);
            if (targetSource != v.sourceKey)
                tuneCents += 1200.0 * std::log2(static_cast<double>(slotHz[(size_t)targetSource]) /
                                                slotHz[(size_t)v.sourceKey]);
        }

        const double baseRate = v.baseRate * (p.correctSamplePitch ? pipe.calibrationRatio : 1.0);
        double rate = baseRate;
        float amp = 1.0f;
        // effects: upper partials split off (sent elsewhere), crackle bursts at the pipe
        const bool split = p.fx.partSpread > 0.5f;
        float *bufH = fxScratch.data();
        const float splitA =
            split ? 1.0f -
                        std::exp(-twoPiOverSr * juce::jmin(0.45f * (float)sr,
                                                           (float)juce::jmax(20.0, pipe.nominalHz) *
                                                               p.fx.partSplit))
                  : 1.0f;
        const float crkLevel = juce::jlimit(0.0f, 1.0f, p.fx.crackLevel + v.pmCrk);
        const bool crackle = crkLevel > 0.0001f && v.crkEnv > 0.001f;
        const float crkDecay =
            std::exp(-1.0f / juce::jmax(1.0f, p.fx.crackLen * 0.001f * (float)sr));
        const float crkMean = (float)sr / juce::jmax(0.05f, p.fx.crackRate);
        const float crkGain = 0.25f * crkLevel * v.crkEnv;
        const bool chaosModel =
            p.windModel == 1 ||
            p.live.pallet > 0.0001f; // (a half-open pallet: the edge tone belongs to it)
        // ---- the sound of this pipe: Ansprache, Abriss, upper partials; glitches
        const PipeSoundParams ps = pipeSndFor(p.pipeSnd[(size_t)v.stop], v);
        const bool attOn = ps.attOn();
        const int attA = pipe.onset, attB = juce::jmax(pipe.onset + 16, pipe.attackEnd);
        const double attInv = 1.0 / (double)(attB - attA);
        const float attBoost = ps.attGain > 0.0f ? 3.0f * ps.attGain : 0.0f,
                    attCut = ps.attGain < 0.0f ? -ps.attGain : 0.0f;
        const float attSharpG = 3.0f * ps.attSharp;
        const float attLpA = 1.0f - std::exp(-twoPiOverSr * 2500.0f);
        const bool chOn = v.chLeft > 0 || v.chAge >= 0;
        const bool hiOn = ps.hiOn();
        // a 2nd-order high-pass (state variable, Butterworth): the layer above hiFreq; out = x +
        // (gain - 1) * layer
        const float hiG0 = (float)std::tan(juce::MathConstants<double>::pi *
                                           juce::jlimit(200.0, 0.45 * sr, (double)ps.hiFreq) / sr);
        const float hiK = 1.41421356f, hiA1 = 1.0f / (1.0f + hiG0 * (hiG0 + hiK)),
                    hiA2 = hiG0 * hiA1, hiA3 = hiG0 * hiA2;
        const float hiGT = juce::Decibels::decibelsToGain(ps.hiGain);
        if (!v.pmNew || v.hiGSm < 0.0f)
            v.hiGSm = hiGT;
        float hiG = v.hiGSm;
        const float airInvT =
            v.pmNew ? juce::jlimit(0.0f, 1.0f, p.airInvert + v.pmx[tgtAirInvert]) : p.airInvert;
        if (!v.pmNew || v.airInvSm < 0.0f)
            v.airInvSm = airInvT;
        float airInv = v.airInvSm;
        const float efA = 1.0f - std::exp(-1.0f / (0.0007f * (float)sr)),
                    efR = 1.0f - std::exp(-1.0f / (0.006f * (float)sr));
        const float esA = 1.0f - std::exp(-1.0f / (0.025f * (float)sr)),
                    esR = 1.0f - std::exp(-1.0f / (0.12f * (float)sr));
        if (v.puffPending)
        {
            // Abriss: a puff of air, shaped by the register and the pitch; as hard as the key was
            // struck
            v.puffPending = false;
            static constexpr float kPuffMult[kNumStops]{4.0f, 2.5f, 4.0f, 4.5f,
                                                        6.0f, 8.0f, 3.0f, 3.0f};
            const double hz =
                pipe.measuredHz > 0.0 ? pipe.measuredHz : juce::jmax(30.0, pipe.nominalHz);
            const float fc = juce::jlimit(250.0f, 12000.0f, (float)hz * kPuffMult[(size_t)v.stop]);
            const float tau = juce::jlimit(0.008f, 0.09f, 0.04f * std::sqrt(130.0f / (float)hz));
            const double w = juce::MathConstants<double>::twoPi * fc / sr,
                         r = std::exp(-juce::MathConstants<double>::pi * fc / (1.4 * sr));
            v.pb1 = (float)(2.0 * r * std::cos(w));
            v.pb2 = (float)(-r * r);
            v.pLpA = 1.0f - std::exp(-twoPiOverSr * juce::jmin(0.45f * (float)sr, 2.5f * fc));
            v.puffDec = std::exp(-1.0f / (tau * (float)sr));
            const float vel = keyVelocity[(size_t)v.key];
            v.puffAmp = ps.puff * clickCurve(vel) * 0.9f *
                        juce::Decibels::decibelsToGain(juce::jlimit(-60.0f, 0.0f, pipe.levelDb)) *
                        (1.0f - (float)r);
            v.puffEnv = 1.0f;
        }
        const bool puffOn = v.puffEnv > 1.0e-4f;
        const GlitchParams gl = glitchFor(p.glitch, v);
        const bool glOn = gl.on();
        const int xfEff = gl.splice > 0.0f
                              ? juce::jmax(1, (int)((float)v.loopXf * (1.0f - gl.splice)))
                              : v.loopXf;
        for (int i = 0; i < n; ++i)
        {
            if ((i & 15) == 0) // control rate: every 16 samples
            {
                if (v.pmNew)
                {
                    // modulated upper-partial level and air inversion glide (about 4 ms) instead of
                    // stepping per block
                    v.hiGSm += 0.08f * (hiGT - v.hiGSm);
                    hiG = v.hiGSm;
                    v.airInvSm += 0.08f * (airInvT - v.airInvSm);
                    airInv = v.airInvSm;
                }
                // stop wind: a half-drawn stop starves the pipe – quieter, flatter, unsteady,
                // duller
                if (v.windSm < 0.0f)
                    v.windSm = windTarget;
                v.windSm += 0.03f * (windTarget - v.windSm);
                const float w = v.windSm, lack = 1.0f - w;
                double cents = tuneCents - 75.0f * lack * std::sqrt(lack);
                float a = std::pow(w, 1.6f);
                if (lack > 0.001f)
                {
                    v.breath.advance(dt * 16.0 * (1.0 + 3.0 * lack));
                    const float br = v.breath.value();
                    cents += 35.0f * lack * lack * br;
                    a *= 1.0f + 0.4f * lack * br;
                }
                v.lpA =
                    w > 0.995f ? 1.0f : 1.0f - std::exp(-twoPiOverSr * (250.0f + 16000.0f * w * w));

                if (wobAmt > 0.0001f)
                {
                    v.wobble.advance(dt * 16.0 * wobSpeed);
                    const float wv = v.wobble.value();
                    cents += wobCents * wv;
                    a *= juce::jmax(0.0f, 1.0f + wobLevel * wv);
                }
                if (turb > 0.0f)
                {
                    const float g = gust[(size_t)(i >> 4)];
                    cents += turbCents * g;
                    a *= juce::jmax(0.05f, 1.0f + turbAmp * g);
                }
                if (tremCents > 0.0f)
                {
                    const float t = trem[(size_t)(i >> 4)];
                    cents += tremCents * t;
                    a *= 1.0f + tremAmp * t;
                }
                v.dop += 0.08f * (v.dopTarget - v.dop);
                cents += v.modCents0 + (v.modCents1 - v.modCents0) * (float)(start + i) * inv;
                if (mpeNow)
                {
                    v.bendSm += 0.2f * (voiceBendTarget(v) - v.bendSm);
                    cents += v.bendSm;
                }
                rate = baseRate * std::exp2(cents / 1200.0f) * v.dop;
                if (chaosModel)
                {
                    // wind model 2: the starving pipe's edge tone (steady at steady wind, lower and
                    // quieter as it falls)
                    const double hz = pipe.measuredHz > 0.0 ? pipe.measuredHz : pipe.nominalHz;
                    v.chaos.control(w, (float)(hz * rate / v.baseRate), v.stop == 1, v.stop == 6,
                                    v.held || v.frozen, (float)sr);
                    a *= v.chaos.toneGain();
                }
                amp = a;
            }

            float x = 0.0f;
            if (!v.frozen || v.freezeMix < 1.0f)
            {
                if (v.looping && v.pos >= pipe.loopEnd - xfEff)
                {
                    const float t = (float)((v.pos - (pipe.loopEnd - xfEff)) / xfEff);
                    x = (1.0f - t) * readSample(d, len, v.pos) +
                        t * readSample(d, len, v.pos - loopLen);
                }
                else
                    x = readSample(d, len, v.pos);

                bool jumped = false;
                if (v.relXfade)
                {
                    const float g = (float)v.relCount / (float)v.relLen;
                    x = x * (1.0f - g) + readSample(d, len, v.relPos) * g;
                    v.relPos += rate;
                    if (++v.relCount >= v.relLen)
                    {
                        // crossfade done: continue from the release reader, no more looping
                        v.relXfade = false;
                        v.looping = false;
                        v.pos = v.relPos;
                        jumped = true;
                    }
                }
                if (!jumped)
                {
                    v.pos += rate;
                    if (v.looping && v.pos >= pipe.loopEnd)
                    {
                        v.pos -= loopLen;
                        if (gl.splice > 0.0f)
                            v.splLeft = (int)(0.012 * sr); // a tape splice: a tick and a short dip
                    }
                }
                if (v.splLeft > 0)
                {
                    const int total = (int)(0.012 * sr), age = total - v.splLeft--;
                    const float tick = age < (int)(0.0025 * sr)
                                           ? std::exp(-(float)age / (0.0006f * (float)sr))
                                           : 0.0f;
                    const float level =
                        juce::Decibels::decibelsToGain(juce::jlimit(-60.0f, 0.0f, pipe.levelDb));
                    x = x * (1.0f - 0.85f * gl.splice * (1.0f - (float)age / (float)total)) +
                        gl.splice * 1.5f * level * tick * v.grainRng.bi();
                }
                if (glOn && !v.relXfade)
                {
                    // glitch: stutter - a short slice of the pipe repeats a few times (hard jumps
                    // back)
                    if (v.stCount > 0)
                    {
                        if (++v.stAcc >= v.stLen)
                        {
                            v.stAcc = 0;
                            v.pos = v.stStart;
                            --v.stCount;
                        }
                    }
                    else if (gl.stutter > 0.0f && --v.glTimer <= 0)
                    {
                        const float u = juce::jmax(1.0e-4f, v.grainRng.uni());
                        v.glTimer = 1 + (int)(-std::log(u) * (float)sr /
                                              juce::jmax(0.02f, 2.5f * gl.stutter * gl.stutter));
                        v.stStart = v.pos;
                        v.stLen = (int)((0.02f + 0.13f * v.grainRng.uni()) * (float)sr);
                        v.stCount = 1 + (int)(v.grainRng.uni() * 5.0f);
                        v.stAcc = 0;
                    }
                }
                if (attOn && v.pos < attB + 0.05 * kSampleFileRate)
                {
                    // Ansprache: e = 1 at the onset, falling to 0 at the end of the measured attack
                    const double u = (v.pos - attA) * attInv;
                    const float e =
                        u <= 0.0
                            ? 1.0f
                            : (u >= 1.0 ? 0.0f
                                        : 0.5f + 0.5f * (float)std::cos(
                                                            juce::MathConstants<double>::pi * u));
                    v.attLp += attLpA * (x - v.attLp);
                    x += attSharpG * e * (x - v.attLp);
                    x *= (1.0f + attBoost * e) * (1.0f - attCut * e);
                }
                if (ps.attIso > 0.0f)
                {
                    const double u = (v.pos - attA) * attInv;
                    const float e =
                        u <= 0.0
                            ? 1.0f
                            : (u >= 1.0 ? 0.0f
                                        : 0.5f + 0.5f * (float)std::cos(
                                                            juce::MathConstants<double>::pi * u));
                    x *= 1.0f - ps.attIso * (1.0f - e);
                }
                if (chOn)
                {
                    // the chiff once more, and again: a short burst (each repeat a little quieter)
                    if (v.chAge < 0 && v.chLeft > 0 && --v.chWait <= 0)
                    {
                        v.chAge = 0;
                        v.chPos = attA;
                        --v.chLeft;
                        ++v.chCount;
                    }
                    if (v.chAge >= 0)
                    {
                        const int lenOut = juce::jmax(16, (int)((double)v.chLen / rate));
                        const float w = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi *
                                                               (float)v.chAge / (float)lenOut);
                        x += readSample(d, len, v.chPos) * w * std::pow(0.82f, (float)v.chCount) *
                             (1.0f + attBoost);
                        v.chPos += rate;
                        if (++v.chAge >= lenOut)
                        {
                            v.chAge = -1;
                            v.chWait = (int)((0.55f + 0.4f * v.grainRng.uni()) * (float)lenOut);
                        }
                    }
                }
            }
            if (v.frozen)
            {
                const float gs = grainSample(v, rate, p.freezeSmear);
                if (v.freezeMix < 1.0f)
                    v.freezeMix = juce::jmin(1.0f, v.freezeMix + freezeStep);
                x = x * (1.0f - v.freezeMix) + gs * v.freezeMix;
            }

            if (v.killing)
            {
                v.killGain -= v.killStep;
                if (v.killGain <= 0.0f)
                {
                    finished = true;
                    x = 0.0f;
                }
                x *= juce::jmax(0.0f, v.killGain);
            }

            v.lp += v.lpA * (x - v.lp);
            v.lpAir += v.airA * (v.lp - v.lpAir);
            // air inversion: the highs the distance took away given back (0.5) or more (1)
            x = (airInv > 0.0f ? v.lpAir + 2.0f * airInv * (v.lp - v.lpAir) : v.lpAir) * amp;
            if (chaosModel && !v.chaos.idle)
            {
                v.chAir += v.airA * (kChaosLevel * v.chaos.sample() - v.chAir);
                x += v.chAir * (v.killing ? juce::jmax(0.0f, v.killGain) : 1.0f);
            }

            if (perPipeDeg)
            {
                const auto f = v.deg.frame(degV, sr);
                x = DegradeState::process(x, v.held1, v.env, f, degV);
            }

            if (hiOn)
            {
                // upper partials as their own layer: gain, and a transient shaper (fast / slow
                // envelope)
                const float v3 = x - v.hiS2, v1 = hiA1 * v.hiS1 + hiA2 * v3,
                            v2 = v.hiS2 + hiA2 * v.hiS1 + hiA3 * v3;
                v.hiS1 = 2.0f * v1 - v.hiS1;
                v.hiS2 = 2.0f * v2 - v.hiS2;
                const float h = x - hiK * v1 - v2, ah = std::abs(h);
                v.hiEf += (ah > v.hiEf ? efA : efR) * (ah - v.hiEf);
                v.hiEs += (ah > v.hiEs ? esA : esR) * (ah - v.hiEs);
                const float tg =
                    ps.hiShape != 0.0f
                        ? juce::jlimit(
                              0.2f, 5.0f,
                              std::pow((v.hiEf + 1.0e-7f) / (v.hiEs + 1.0e-7f), 1.5f * ps.hiShape))
                        : 1.0f;
                x += h * (hiG * tg - 1.0f);
            }
            if (glOn && gl.clip > 0.0f)
            {
                // glitch: a short burst of hard clipping
                if (v.clipLeft > 0)
                {
                    --v.clipLeft;
                    const float lim = 0.02f;
                    x = juce::jlimit(-lim, lim, x * v.clipDrive) * (1.5f / std::sqrt(v.clipDrive));
                }
                else if (--v.clipTimer <= 0)
                {
                    const float u = juce::jmax(1.0e-4f, v.grainRng.uni());
                    v.clipTimer = 1 + (int)(-std::log(u) * (float)sr /
                                            juce::jmax(0.02f, 3.0f * gl.clip * gl.clip));
                    v.clipLeft = (int)((0.008f + 0.07f * v.grainRng.uni()) * (float)sr);
                    v.clipDrive = 4.0f + 30.0f * gl.clip * v.grainRng.uni();
                }
            }
            if (puffOn)
            {
                // Abriss: band-passed noise with a fast decay, at the pipe
                const float nz = v.grainRng.bi();
                v.pLp += v.pLpA * (nz - v.pLp);
                const float y = v.pLp + v.pb1 * v.py1 + v.pb2 * v.py2;
                v.py2 = v.py1;
                v.py1 = y;
                x += v.puffAmp * v.puffEnv * y * (v.killing ? juce::jmax(0.0f, v.killGain) : 1.0f);
                v.puffEnv *= v.puffDec;
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
                    const float u = juce::jmax(1.0e-4f, v.grainRng.uni());
                    v.crkCount = 1 + (int)(-std::log(u) * crkMean);
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

            if (v.previewRemain > 0 && --v.previewRemain == 0)
                releaseVoice(v);
            if (!v.frozen && v.pos >= len - 2)
            {
                finished = true;
                for (int j = i + 1; j < n; ++j)
                {
                    buf[j] = 0.0f;
                    bufH[j] = 0.0f;
                }
                break;
            }
            if (finished)
            {
                for (int j = i + 1; j < n; ++j)
                {
                    buf[j] = 0.0f;
                    bufH[j] = 0.0f;
                }
                break;
            }
        }

        float *bufRamp = scratchRamp.data();
        if (v.hard)
        {
            // hard jumps: constant gains between the jumps, switched at the exact sample (audible
            // clicks at high speed)
            const double ph0 = phasePrev[(size_t)v.stop], dph = phaseNow[(size_t)v.stop] - ph0;
            const double off = jumpOffset((uint32_t)(v.stop * 101 + v.key * 7 + 1));
            std::array<float, kMaxAmbiChannels> g{}, gh{};
            int i = start;
            while (i < end)
            {
                const double phI = ph0 + dph * (double)i / (double)blockLen;
                const double cell = std::floor(phI + off);
                int j = end;
                if (dph > 1.0e-12)
                    j = (int)std::ceil((cell + 1.0 - off - ph0) * (double)blockLen / dph);
                else if (dph < -1.0e-12)
                    j = (int)std::floor((cell - off - ph0) * (double)blockLen / dph) + 1;
                j = juce::jlimit(i + 1, end, j);
                hardJumpGains(v, p, cell + 0.5 - off, g.data(), split ? gh.data() : nullptr);
                mixInto(outs, i, usedChannels, buf + (i - start), nullptr, g.data(), nullptr,
                        j - i);
                if (split)
                    mixInto(outs, i, usedChannels, bufH + (i - start), nullptr, gh.data(), nullptr,
                            j - i);
                i = j;
            }
            if (split)
                juce::FloatVectorOperations::add(buf, bufH, n);
            for (int k2 = 0; k2 < n; ++k2)
                bufRamp[k2] = buf[k2] * (float)k2;
        }
        else
        {
            // add to the output with the gain ramp of this block:
            // o += buf * (ga + d*i)  ==  o += ga*buf + d*(buf*i)   (two vectorised passes per
            // channel)
            for (int i = 0; i < n; ++i)
                bufRamp[i] = buf[i] * (float)i;
            // out of sight: the direct sound through its own low-pass (the room sends below keep
            // 'buf')
            float *dirBuf = buf;
            float *dirRamp = bufRamp;
            // (its strength glides over ~30 ms: switched on or off at once the duller signal would
            // jump to the clear one)
            const float hA0 = v.hidAc,
                        hA1 = hA0 +
                              (v.hidA - hA0) * (1.0f - std::exp(-(float)n / (0.03f * (float)sr)));
            v.hidAc = hA1 > 0.9995f && v.hidA >= 1.0f ? 1.0f : hA1;
            if ((hA0 < 1.0f || v.hidAc < 1.0f) && (int)hidBuf.size() >= 2 * n)
            {
                dirBuf = hidBuf.data();
                dirRamp = hidBuf.data() + hidBuf.size() / 2;
                float s = v.hidLp;
                const float da = (v.hidAc - hA0) / (float)juce::jmax(1, n);
                for (int i = 0; i < n; ++i)
                {
                    const float a = hA0 + da * (float)(i + 1);
                    s += a * (buf[i] - s);
                    dirBuf[i] = s;
                    dirRamp[i] = s * (float)i;
                }
                v.hidLp = s;
            }
            else if (n > 0)
                v.hidLp = buf[n - 1]; // (follows, so hiding starts without a click)
            float ga[kMaxAmbiChannels], dg[kMaxAmbiChannels];
            for (int c = 0; c < usedChannels; ++c)
            {
                const float a = v.g0[(size_t)c], b = v.g1[(size_t)c];
                ga[c] = a + (b - a) * (float)start * inv;
                dg[c] = (b - a) * inv;
            }
            mixInto(outs, start, usedChannels, dirBuf, dirRamp, ga, dg, n);
            if (v.near.q0.gL + v.near.q0.gR + v.near.q1.gL + v.near.q1.gR > 1.0e-7f &&
                (int)nearL.size() >= start + n)
            {
                // Fluestern: the same signal straight to the two ears
                auto &N = v.near;
                const auto &a = N.q0, &b = N.q1;
                float *nl = nearL.data() + start, *nr = nearR.data() + start;
                const float aLow = 1.0f - std::exp(-twoPiOverSr * 180.0f);
                for (int i = 0; i < n; ++i)
                {
                    const float t = (float)(start + i + 1) * inv;
                    N.lp += aLow * (dirBuf[i] - N.lp);
                    N.hist[(size_t)N.w] = dirBuf[i] + 0.9f * (a.bass + (b.bass - a.bass) * t) * N.lp;
                    auto tap = [&](float d)
                    {
                        const int di = (int)d;
                        const float fr = d - (float)di;
                        return N.hist[(size_t)((N.w - di) & 127)] * (1.0f - fr) +
                               N.hist[(size_t)((N.w - di - 1) & 127)] * fr;
                    };
                    N.sL += (a.aL + (b.aL - a.aL) * t) * (tap(a.dL + (b.dL - a.dL) * t) - N.sL);
                    N.sR += (a.aR + (b.aR - a.aR) * t) * (tap(a.dR + (b.dR - a.dR) * t) - N.sR);
                    nl[i] += (a.gL + (b.gL - a.gL) * t) * N.sL;
                    nr[i] += (a.gR + (b.gR - a.gR) * t) * N.sR;
                    N.w = (N.w + 1) & 127;
                }
                nearUsed = true;
            }
            if (split)
            {
                // upper partials with their own direction; afterwards both parts go to the room
                // together
                float *bufHRamp = fxScratch.data() + maxBlock;
                for (int i = 0; i < n; ++i)
                    bufHRamp[i] = bufH[i] * (float)i;
                for (int c = 0; c < usedChannels; ++c)
                {
                    const float a = v.gh0[(size_t)c], b = v.gh1[(size_t)c];
                    ga[c] = a + (b - a) * (float)start * inv;
                    dg[c] = (b - a) * inv;
                }
                mixInto(outs, start, usedChannels, bufH, bufHRamp, ga, dg, n);
                juce::FloatVectorOperations::add(buf, bufH, n);
                juce::FloatVectorOperations::add(bufRamp, bufHRamp, n);
            }
        }
        if (v.cloudRing >= 0 || cloudFor(p.cloud[(size_t)v.stop], v).on())
            cloudWrite(v, buf, nullptr, n, p);
        if (sympBusOn)
            juce::FloatVectorOperations::addWithMultiply(sympBus.data() + start, buf, v.gainLin,
                                                         n); // what the silent pipes hear
        {
            // room sends: plain sum + circular harmonics of the pipe direction (start / end of the
            // slice)
            const double a0 = juce::degreesToRadians((double)v.sendAz0),
                         a1 = juce::degreesToRadians((double)v.sendAz1);
            const double c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
            double cm0 = 1.0, sm0 = 0.0, cm1 = 1.0, sm1 = 0.0; // cos / sin (m * angle), m = 0
            float sendA[kMaxRoomSends], sendD[kMaxRoomSends];
            float *sendDst[kMaxRoomSends];
            for (int k = 0; k < roomSends; ++k)
            {
                float w0 = 1.0f, w1 = 1.0f;
                if (k > 0)
                {
                    if (k % 2 == 1)
                    {
                        // next m: rotate (cos, sin) by the angle
                        const double t0 = cm0 * c0 - sm0 * s0;
                        sm0 = sm0 * c0 + cm0 * s0;
                        cm0 = t0;
                        const double t1 = cm1 * c1 - sm1 * s1;
                        sm1 = sm1 * c1 + cm1 * s1;
                        cm1 = t1;
                        w0 = (float)cm0;
                        w1 = (float)cm1;
                    }
                    else
                    {
                        w0 = (float)sm0;
                        w1 = (float)sm1;
                    }
                }
                const float g0 = v.send0 * w0, g1 = v.send1 * w1;
                sendA[k] = g0 + (g1 - g0) * (float)start * inv;
                sendD[k] = (g1 - g0) * inv;
                sendDst[k] = roomSend[(size_t)k].data() + roomOffset;
            }
            mixInto(sendDst, start, roomSends, buf, bufRamp, sendA, sendD, n, 1.0e-12f);
            // a stop in another simulated room: its late reverb gets the same send
            const int vr = juce::jlimit(0, kNumRooms - 1, p.stopRoom[(size_t)v.stop]);
            if (vr > 0 && roomOnNow)
            {
                const float sa = v.send0 + (v.send1 - v.send0) * (float)start * inv,
                            sd = (v.send1 - v.send0) * inv;
                float *dst = rt[(size_t)vr].send.data() + roomOffset + start;
                if (std::abs(sa) > 1.0e-12f)
                    juce::FloatVectorOperations::addWithMultiply(dst, buf, sa, n);
                if (std::abs(sd) > 1.0e-12f)
                    juce::FloatVectorOperations::addWithMultiply(dst, bufRamp, sd, n);
            }
        }
        if (v.erSlot >= 0)
        {
            float *ring = erRing.data() + (size_t)v.erSlot * (size_t)kErRing;
            const uint32_t base = erClock + (uint32_t)start;
            for (int i = 0; i < n; ++i)
                ring[(base + (uint32_t)i) & (kErRing - 1)] += buf[i];
        }
        if (finished)
        {
            v.active = false;
            releaseSlot(v.erSlot);
        }
    }
    renderCloud(outs, start, n);

    // noise one-shots
    for (auto &nv : noise)
    {
        if (!nv.active)
            continue;
        float *buf = scratch.data();
        int m = 0;
        bool ended = false;
        if (nv.resN == 0)
        {
            for (; m < n; ++m)
            {
                if (nv.pos >= nv.length - 2)
                {
                    ended = true;
                    break;
                }
                nv.lp += nv.lpA * (readSample(nv.data, nv.length, nv.pos) - nv.lp);
                buf[m] = nv.lp * nv.gain;
                nv.pos += nv.rate;
            }
        }
        else
        {
            // resonated click: the click excites the key's harmonics, which ring on after it
            const float dry = 1.0f - 0.6f * nv.resMix;
            for (; m < n; ++m)
            {
                float x = 0.0f;
                if (nv.pos < nv.length - 2)
                {
                    nv.lp += nv.lpA * (readSample(nv.data, nv.length, nv.pos) - nv.lp);
                    x = nv.lp * nv.gain;
                    nv.pos += nv.rate;
                }
                else if (--nv.resTail <= 0)
                {
                    ended = true;
                    break;
                }
                float r = 0.0f;
                for (int k = 0; k < nv.resN; ++k)
                {
                    const float y = nv.rg[(size_t)k] * x + nv.rb1[(size_t)k] * nv.ry1[(size_t)k] +
                                    nv.rb2[(size_t)k] * nv.ry2[(size_t)k];
                    nv.ry2[(size_t)k] = nv.ry1[(size_t)k];
                    nv.ry1[(size_t)k] = y;
                    r += y;
                }
                buf[m] = dry * x + nv.resMix * r;
            }
        }
        if (nv.erSlot >= 0)
        {
            float *ring = erRing.data() + (size_t)nv.erSlot * (size_t)kErRing;
            for (int i = 0; i < m; ++i)
                ring[(erClock + (uint32_t)(start + i)) & (kErRing - 1)] += buf[i];
        }
        mixInto(outs, start, usedChannels, buf, nullptr, nv.g.data(), nullptr, m);
        float sendW[kMaxRoomSends], sendZero[kMaxRoomSends]{};
        float *sendDst[kMaxRoomSends];
        for (int k = 0; k < roomSends; ++k)
        {
            sendW[k] = nv.send * nv.sendW[(size_t)k];
            sendDst[k] = roomSend[(size_t)k].data() + roomOffset;
        }
        mixInto(sendDst, start, roomSends, buf, nullptr, sendW, sendZero, m, 0.0f);
        if (ended)
        {
            nv.active = false;
            releaseSlot(nv.erSlot);
        }
    }

    // wind: omnidirectional (W) in Ambisonics, centre in stereo
    if (library != nullptr && !library->wind.empty())
    {
        const auto &w = *library->wind.front();
        const int wl = w.size();
        const float target = p.wind * 2.0f;
        const double wr = w.sampleRate / sr;
        if (wl > 4 && (target > 0.0001f || windGain > 0.0001f) && p.fx.windRound)
        {
            // four uncorrelated parts of the recording from four directions (tetrahedron): the
            // blower is all around
            static const float dirs[4][2]{
                {45.0f, 35.0f}, {135.0f, -35.0f}, {-135.0f, 35.0f}, {-45.0f, -35.0f}};
            std::array<std::array<float, kMaxAmbiChannels>, 4> wg{};
            for (int d = 0; d < 4; ++d)
                gainsFor({dirs[d][0], dirs[d][1], kRefDist}, wg[(size_t)d].data());
            const double quarter = (double)(wl - 2) / 4.0;
            for (int i = 0; i < n; ++i)
            {
                windGain += 0.0005f * (target - windGain);
                const float gst = turb > 0.0f ? gust[(size_t)(i >> 4)] : 0.0f;
                const float tr = p.tremOn ? 0.5f * p.tremDepth * trem[(size_t)(i >> 4)] : 0.0f;
                const float g =
                    0.5f * windGain * juce::jmax(0.0f, 1.0f + 1.6f * turb * gst + 1.5f * turb + tr);
                for (int d = 0; d < 4; ++d)
                {
                    double ps = windPos + quarter * d;
                    if (ps >= wl - 2)
                        ps -= (wl - 2);
                    const float x = readSample(w.data.data(), wl, ps) * g;
                    for (int c = 0; c < usedChannels; ++c)
                        out.getWritePointer(c)[start + i] += x * wg[(size_t)d][(size_t)c];
                }
                windPos += wr;
                if (windPos >= wl - 2)
                    windPos -= (wl - 2);
            }
        }
        else if (wl > 4 && (target > 0.0001f || windGain > 0.0001f))
        {
            for (int i = 0; i < n; ++i)
            {
                windGain += 0.0005f * (target - windGain);
                const float gst = turb > 0.0f ? gust[(size_t)(i >> 4)] : 0.0f;
                const float tr = p.tremOn ? 0.5f * p.tremDepth * trem[(size_t)(i >> 4)] : 0.0f;
                float x = readSample(w.data.data(), wl, windPos) * windGain *
                          juce::jmax(0.0f, 1.0f + 1.6f * turb * gst + 1.5f * turb + tr);
                windPos += wr;
                if (windPos >= wl - 2)
                    windPos -= (wl - 2);
                if (order == 0)
                {
                    out.getWritePointer(0)[start + i] += x * 0.7071f;
                    if (usedChannels > 1)
                        out.getWritePointer(1)[start + i] += x * 0.7071f;
                }
                else
                    out.getWritePointer(0)[start + i] += x;
            }
        }
    }
    // difference tones of the sounding pairs ("Differenzt\u00f6ne")
    if (p.ghostLevel > 0.0f || ghostsNow.load() > 0)
        renderGhosts(outs, start, n, inv);
}

// ---------------------------------------------------------------------------
// tuning made audible in space: beats between the sounding pipes, and their difference tones
// ---------------------------------------------------------------------------
void OrganEngine::updateBeats(const EngineParams &p, double blockSeconds) noexcept
{
    // the sounding pipes
    int idx[kMaxVoices];
    int nv = 0;
    for (int i = 0; i < (int)voices.size(); ++i)
    {
        auto &v = voices[(size_t)i];
        v.beatT = 0.0f;
        if (v.active && !v.killing && v.hzNow > 8.0f && v.gainLin > 1.0e-4f && nv < kMaxVoices)
            idx[nv++] = i;
    }
    bool anyBeatMotion = false;
    for (int s = 0; s < kNumStops; ++s)
        anyBeatMotion = anyBeatMotion || (Motion)p.motionMode[(size_t)s] == Motion::Beat;
    const bool ghostsOn = p.ghostLevel > 0.0f;
    bool beatRouted = false;
    for (const auto &r : p.mod.routes)
        beatRouted = beatRouted || (r.src == srcBeat && r.amount != 0.0f);
    if (!anyBeatMotion && !ghostsOn && !beatRouted && ghostsNow.load() == 0)
    {
        // nothing listens: no pairs to compare
        for (auto &v : voices)
        {
            v.beatHz = 0.0f;
            v.beatNorm = 0.0f;
        }
        beatMaxNow = 0.0f;
        return;
    }
    // the strongest pairs for difference tones
    struct Cand
    {
        float w = 0.0f, hz = 0.0f;
        int a = -1, b = -1;
    };
    std::array<Cand, kGhosts> best{};
    for (int ia = 0; ia < nv; ++ia)
        for (int ib = ia + 1; ib < nv; ++ib)
        {
            auto &A = voices[(size_t)idx[ia]];
            auto &B = voices[(size_t)idx[ib]];
            const bool aLow = A.hzNow <= B.hzNow;
            const float f1 = aLow ? A.hzNow : B.hzNow, f2 = aLow ? B.hzNow : A.hzNow;
            // beats: the lowest pair of partials that nearly coincide (n * f1 ~ m * f2), slower
            // than 35 Hz
            int bestNm = 1000;
            float beat = -1.0f;
            for (int m = 1; m <= 6; ++m)
            {
                const int nn = (int)std::lround((double)m * f2 / f1);
                if (nn < 1 || nn > 8 || nn * m >= bestNm)
                    continue;
                const float b = std::abs((float)nn * f1 - (float)m * f2);
                if (b < 35.0f)
                {
                    bestNm = nn * m;
                    beat = b;
                }
            }
            if (beat >= 0.0f)
            {
                A.beatT = juce::jmax(A.beatT, beat);
                B.beatT = juce::jmax(B.beatT, beat);
            }
            if (ghostsOn && A.held && B.held)
            {
                // the difference tone (below the lower pipe, as the resultant 32' of the organ
                // builders)
                const float fd = f2 - f1;
                if (fd >= 16.0f && fd <= f1 * 1.02f)
                {
                    const float cons = beat >= 0.0f ? juce::jmin(1.0f, 6.0f / (float)bestNm) : 0.0f;
                    const float w = std::sqrt(A.gainLin * B.gainLin) *
                                    (1.0f - p.ghostCons + p.ghostCons * cons);
                    if (w > 1.0e-4f)
                    {
                        int k = kGhosts - 1;
                        if (w > best[(size_t)k].w)
                        {
                            while (k > 0 && best[(size_t)(k - 1)].w < w)
                            {
                                best[(size_t)k] = best[(size_t)(k - 1)];
                                --k;
                            }
                            best[(size_t)k] = {w, fd, idx[ia], idx[ib]};
                        }
                    }
                }
            }
        }
    // smoothed beat rates; the "Schwebung" motion turns every pipe by its own beat
    const float k = 1.0f - std::exp(-(float)blockSeconds / 0.25f);
    float maxBeat = 0.0f;
    for (int i = 0; i < nv; ++i)
    {
        auto &v = voices[(size_t)idx[i]];
        v.beatHz += k * (v.beatT - v.beatHz);
        v.beatNorm = juce::jlimit(0.0f, 1.0f, std::log2(1.0f + v.beatHz) / 5.044f); // 32 Hz = 1
        maxBeat = juce::jmax(maxBeat, v.beatHz);
        if (anyBeatMotion && (Motion)p.motionMode[(size_t)v.stop] == Motion::Beat && v.held)
        {
            auto &t = beatTurn[(size_t)v.stop][(size_t)v.key];
            float a = t.load() + (float)(360.0 * (double)p.motionRate[(size_t)v.stop] *
                                         (double)v.beatHz * blockSeconds);
            a -= 360.0f * std::floor(a / 360.0f);
            t = a;
        }
    }
    for (auto &v : voices)
        if (!v.active)
        {
            v.beatHz = 0.0f;
            v.beatNorm = 0.0f;
        }
    beatMaxNow = maxBeat;

    // difference tones: keep a ghost on its pair (continuous phase), new pairs get free ghosts,
    // gone pairs fade
    for (auto &g : ghosts)
    {
        g.seen = false;
        std::copy(g.g1.begin(), g.g1.end(), g.g0.begin());
    }
    int live = 0;
    for (const auto &c : best)
    {
        if (c.a < 0)
            continue;
        const auto &A = voices[(size_t)c.a];
        const auto &B = voices[(size_t)c.b];
        Ghost *g = nullptr;
        for (auto &x : ghosts)
            if (x.on &&
                ((x.ageA == A.age && x.ageB == B.age) || (x.ageA == B.age && x.ageB == A.age)))
            {
                g = &x;
                break;
            }
        if (g == nullptr)
            for (auto &x : ghosts)
                if (!x.on)
                {
                    g = &x;
                    break;
                }
        if (g == nullptr)
            continue;
        if (!g->on)
        {
            *g = Ghost{};
            g->on = true;
            g->ageA = A.age;
            g->ageB = B.age;
            g->hz = c.hz;
        }
        g->seen = true;
        g->hzT = c.hz;
        g->ampT = 0.06f * p.ghostLevel * c.w;
        // where: between the two pipes (direction of the sum of their unit vectors), towards the
        // floor with 'place'
        auto unit = [](const Pos &q, float &x, float &y, float &z)
        {
            const float a = juce::degreesToRadians(q.az), e = juce::degreesToRadians(q.el);
            x = -std::sin(a) * std::cos(e);
            y = std::cos(a) * std::cos(e);
            z = std::sin(e);
        };
        float ax, ay, az, bx, by, bz;
        unit(A.spPos, ax, ay, az);
        unit(B.spPos, bx, by, bz);
        const float sx = ax + bx, sy = ay + by, sz = az + bz;
        Pos gp;
        gp.az = std::abs(sx) + std::abs(sy) > 1.0e-4f ? juce::radiansToDegrees(std::atan2(-sx, sy))
                                                      : A.spPos.az;
        const float elMid = juce::radiansToDegrees(std::atan2(sz, std::sqrt(sx * sx + sy * sy)));
        gp.el = juce::jlimit(-89.0f, 89.0f, elMid + p.ghostPlace * (-75.0f - elMid));
        gp.dist = juce::jlimit(kMinDist, kMaxDist,
                               0.5f * (A.spPos.dist + B.spPos.dist) * (1.0f + 0.5f * p.ghostPlace));
        float gg[kMaxAmbiChannels];
        gainsFor(gp, gg);
        const float dg = distanceGain(gp.dist, p.distAmount);
        for (int ch = 0; ch < kMaxAmbiChannels; ++ch)
            g->g1[(size_t)ch] = gg[ch] * dg;
        if (g->amp <= 0.0f)
            std::copy(g->g1.begin(), g->g1.end(),
                      g->g0.begin()); // a new ghost: no sweep from nowhere
    }
    for (auto &g : ghosts)
    {
        if (!g.on)
            continue;
        if (!g.seen)
            g.ampT = 0.0f;
        if (g.ampT <= 0.0f && g.amp < 1.0e-6f)
        {
            g.on = false;
            continue;
        }
        ++live;
    }
    ghostsNow = live;
}

void OrganEngine::renderGhosts(float *const *outs, int start, int n, float inv) noexcept
{
    float *y = fxScratch.data() + 10 * (size_t)maxBlock;
    float *yr = fxScratch.data() + 11 * (size_t)maxBlock;
    // fade in over ~0.4 s once the interval is held, out over ~0.3 s
    const float upA = 1.0f - std::exp(-1.0f / (0.4f * (float)sr)),
                downA = 1.0f - std::exp(-1.0f / (0.3f * (float)sr));
    const float glideA = 1.0f - std::exp(-1.0f / (0.03f * (float)sr));
    const double twoPi = juce::MathConstants<double>::twoPi;
    for (auto &g : ghosts)
    {
        if (!g.on)
            continue;
        for (int i = 0; i < n; ++i)
        {
            g.amp += (g.ampT > g.amp ? upA : downA) * (g.ampT - g.amp);
            g.hz += glideA * (g.hzT - g.hz);
            g.ph += (double)g.hz / sr;
            if (g.ph >= 1.0)
                g.ph -= 1.0;
            // a soft, round tone (a little 2nd and 3rd partial so the low ones are heard on small
            // speakers)
            const double a = twoPi * g.ph;
            const float x =
                (float)(std::sin(a) + 0.18 * std::sin(2.0 * a) + 0.08 * std::sin(3.0 * a)) * g.amp;
            y[i] = x;
            yr[i] = x * (float)i;
        }
        float ga[kMaxAmbiChannels], dg[kMaxAmbiChannels];
        for (int c = 0; c < kMaxAmbiChannels; ++c)
        {
            const float a = g.g0[(size_t)c], b = g.g1[(size_t)c];
            ga[c] = a + (b - a) * (float)start * inv;
            dg[c] = (b - a) * inv;
        }
        mixInto(outs, start, usedChannels, y, yr, ga, dg, n);
    }
}

// ---------------------------------------------------------------------------
// Doppelbelichtung / Belichtung on the finished field
// ---------------------------------------------------------------------------
void OrganEngine::applyPhotoFx(float *const *ch, int outputMode, int n,
                               const EngineParams &p) noexcept
{
    const bool stereo = outputMode == 0;
    const int numCh = stereo ? juce::jmin(2, usedChannels) : (outputMode + 1) * (outputMode + 1);
    dexFx.process(ch, numCh, stereo, n, p.dex);
    expoFx.process(ch, numCh, stereo, n, p.expo);
}

// ---------------------------------------------------------------------------
// simulated room ("Saal"): early reflections (image sources, per sound) and the late tail (FDN)
// ---------------------------------------------------------------------------
int OrganEngine::allocSlot(int voice, int noiseVoice) noexcept
{
    int best = -1, bestDrain = std::numeric_limits<int>::max();
    for (int i = 0; i < (int)erSlots.size(); ++i)
    {
        auto &s = erSlots[(size_t)i];
        if (!s.active)
        {
            best = i;
            break;
        }
        if (s.voice < 0 && s.noiseVoice < 0 && s.drain < bestDrain)
        {
            bestDrain = s.drain;
            best = i;
        } // ringing out: the oldest may go
    }
    if (best < 0)
        return -1; // all busy: this sound gets no own reflections (the tail still has it)
    auto &s = erSlots[(size_t)best];
    s.active = true;
    s.fresh = true;
    s.voice = voice;
    s.noiseVoice = noiseVoice;
    s.input = -1;
    s.drain = 0;
    s.numTaps = 0;
    for (auto &t : s.taps)
    {
        t.lp = 0.0f;
        t.code = -1;
        t.vis = t.visT = 0.0f;
        t.hold = t.born = false;
    }
    s.visValid = false;
    s.fading = false;
    s.visShape = -1;
    s.visN = -1;
    std::fill_n(erRing.data() + (size_t)best * (size_t)kErRing, kErRing, 0.0f);
    return best;
}

void OrganEngine::releaseSlot(int &slot) noexcept
{
    if (slot < 0 || slot >= (int)erSlots.size())
    {
        slot = -1;
        return;
    }
    auto &s = erSlots[(size_t)slot];
    s.voice = -1;
    s.noiseVoice = -1;
    s.input = -1;
    float maxD = 0.0f;
    for (int t = 0; t < s.numTaps; ++t)
        maxD = juce::jmax(maxD, s.taps[(size_t)t].d1);
    s.drain = (int)maxD + 512;
    slot = -1;
}

void OrganEngine::updateSlotTaps(ErSlot &s, ErWork &w, Pos srcPos, float base,
                                 const EngineParams &p,
                                 const RoomParams &r, const RoomParams *placeFrom,
                                 const Vec3f *fixedSrc) noexcept
{
    ProfScope profGeo(profOn, w.geoMs);
    if (!r.box())
    {
        updateSlotTapsPlan(s, w, srcPos, base, p, r, placeFrom, fixedSrc);
        return;
    }
    {
        // a source that has not moved in an unchanged room: the taps are still right
        const std::array<float, ErSlot::kKey> key{
            srcPos.az, srcPos.el,      srcPos.dist, base,          r.W(),      r.L(),
            r.H(),     r.listenerX,    r.listenerY, r.absWalls,    r.absFloor, r.highDamp,
            r.scatter, (float)r.order, r.erGain,    distAmountNow, p.air,      (float)order,
            surfKey(r), r.listenerZ,   0.0f};
        if (!s.fresh && s.keyValid && key == s.key)
            return;
        s.key = key;
        s.keyValid = true;
    }
    const Vec3f L = listenerPos(r),
                S = fixedSrc != nullptr ? *fixedSrc
                                        : roomPoint(placeFrom != nullptr ? *placeFrom : r, srcPos);
    const float rd = seenFrom(L, S).dist;
    std::array<ImageSource, kMaxTaps> ims;
    const int n = imageSources(r, S, r.order, ims.data(), kMaxTaps);
    // every surface with its own absorption (walls, floor and ceiling can be of different stuff):
    // the reflection factor and the loss of highs per hit
    const auto ab = SurfAbs::from(r);
    float Rs[kMaxSurf], Sh[kMaxSurf];
    for (int k = 0; k < kMaxSurf; ++k)
    {
        Rs[k] = std::sqrt(1.0f - ab.low[(size_t)k]);
        Sh[k] = std::sqrt((1.0f - ab.high[(size_t)k]) / juce::jmax(1e-4f, 1.0f - ab.low[(size_t)k]));
    }
    const float spec = std::sqrt(
        juce::jlimit(0.0f, 1.0f, 1.0f - r.scatter)); // the scattered part goes to the tail
    const float airDiff = 0.5f * (airHigh(p.air) - airLow());
    const int encOrder = order == 0 ? 0 : juce::jmin(order, 3);
    const float maxDelay = (float)(kErRing - 4 * kSubBlock);
    float tmp[kMaxAmbiChannels];
    const float specPow[4]{1.0f, spec, spec * spec, spec * spec * spec};
    // the walls an image's path meets, from its numbers: along x the left / right wall, along y the
    // back / front wall, along z floor / ceiling
    auto imageLoss = [&](const ImageSource &im, float &g, float &sh)
    {
        g = sh = 1.0f;
        auto hit = [&](int surf, int times)
        {
            for (int q = 0; q < times; ++q)
            {
                g *= Rs[surf];
                sh *= Sh[surf];
            }
        };
        int lo, hi;
        axisHits(im.mx, lo, hi);
        hit(kSurfWall0 + 3, lo);
        hit(kSurfWall0 + 1, hi);
        axisHits(im.my, lo, hi);
        hit(kSurfWall0 + 0, lo);
        hit(kSurfWall0 + 2, hi);
        axisHits(im.mz, lo, hi);
        hit(kSurfFloor, lo);
        hit(kSurfCeiling, hi);
        g *= specPow[juce::jlimit(0, 3, im.hitsWalls + im.hitsFloor)];
    };
    for (int i = 0; i < n; ++i)
    {
        const auto &im = ims[(size_t)i];
        // direction and distance seen from the listener (as seenFrom, without the angles: the
        // encoder only needs their cos / sin)
        const float dx = im.pos.x - L.x, dy = im.pos.y - L.y, dz = im.pos.z - L.z;
        const float rh = std::sqrt(dx * dx + dy * dy);
        Pos seen;
        seen.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const int hw = juce::jlimit(0, 3, im.hitsWalls), hf = juce::jlimit(0, 3, im.hitsFloor);
        float delay = (seen.dist - rd) / kSpeedOfSoundRoom * (float)sr;
        float refl, shelf;
        imageLoss(im, refl, shelf);
        juce::ignoreUnused(hw, hf);
        float gain = base * distanceGain(seen.dist, distAmountNow) * r.erGain * refl;
        gain *= juce::jlimit(0.0f, 1.0f,
                             (maxDelay - delay) /
                                 (0.15f * maxDelay)); // longer than the buffer: left to the tail
                                                      // (faded out before it, no click)
        delay = juce::jmin(delay, maxDelay);
        auto &t = s.taps[(size_t)i];
        t.d1 = juce::jmax(0.0f, delay);
        t.g1 = gain;
        t.r1 = shelf * std::exp(-airDiff * seen.dist);
        std::fill(tmp, tmp + kMaxAmbiChannels, 0.0f);
        // first reflections sharp (up to 3rd order), the later ones at first order: they are many,
        // quieter and more diffuse anyway, and this keeps the cost down
        const int o = (im.hitsWalls + im.hitsFloor == 1) ? encOrder : juce::jmin(encOrder, 1);
        const double cosAz = rh > 1.0e-6f ? dy / rh : 1.0, sinAz = rh > 1.0e-6f ? -dx / rh : 0.0;
        const double sinEl =
            seen.dist > 1.0e-6f ? juce::jlimit(-1.0, 1.0, (double)(dz / seen.dist)) : 0.0;
        const double cosEl = seen.dist > 1.0e-6f ? (double)(rh / seen.dist) : 1.0;
        if (encOrder == 0)
        {
            encodeStereo(juce::radiansToDegrees(std::atan2(-dx, dy)), tmp);
            t.ch = 2;
        }
        else
        {
            encodeAmbiXDir(cosAz, sinAz, sinEl, cosEl, o, tmp);
            t.ch = (o + 1) * (o + 1);
        }
        std::copy(tmp, tmp + kErCh, t.enc.begin());
        if (s.fresh || i >= s.numTaps)
        {
            t.d0 = t.d1;
            t.g0 = t.g1;
            t.r0 = t.r1;
            t.lp = 0.0f;
        }
    }
    for (int i = n; i < s.numTaps; ++i)
        s.taps[(size_t)i].g1 = 0.0f; // order lowered: fade the extra taps out
    s.numTaps = juce::jmax(n, s.fresh ? n : s.numTaps);
    s.fresh = false;
}

/** Early reflections in a polygon room. Finding the visible reflection paths is expensive (a search
   over all wall sequences, with occlusion tests in non-convex rooms), so it runs only when the
   source has moved noticeably, the room changed or a new sound starts - and at most a few times per
   block (the rest wait for the next block). In between, every tap keeps its path and only the image
   position is recomputed (cheap and smooth). Paths that appear or disappear fade in / out over 25
   ms, like a reflection that slides past the end of a wall. */
void OrganEngine::updateSlotTapsPlan(ErSlot &s, ErWork &w, Pos srcPos, float base,
                                     const EngineParams &p,
                                     const RoomParams &r, const RoomParams *placeFrom,
                                     const Vec3f *fixedSrc) noexcept
{
    const std::array<float, ErSlot::kKey> key{
        srcPos.az,      srcPos.el,      srcPos.dist,    base,          r.W(),      r.L(),
        r.H(),          r.listenerX,    r.listenerY,    r.absWalls,    r.absFloor, r.highDamp,
        r.scatter,      (float)r.order, r.erGain,       distAmountNow, p.air,      (float)order,
        (float)r.shape, r.shapeAmt,     (float)r.plan.n, surfKey(r),   r.listenerZ};
    if (!s.fresh && s.keyValid && key == s.key && !s.fading)
        return;
    bool roomChanged = !s.keyValid;
    // Cache identity is exact: even small parameter edits invalidate the room.
    roomChanged = roomChanged || !std::equal(key.begin() + 4, key.end(), s.key.begin() + 4);
    s.key = key;
    s.keyValid = true;

    // (one of the rooms of this slice: its walls and the listener's place are known already)
    const ptrdiff_t known = &r - roomsEr.data();
    const bool isKnown = known >= 0 && known < (ptrdiff_t)kNumRooms;
    PlanGeo gOwn;
    if (!isKnown)
        gOwn = planGeo(r);
    const PlanGeo &g = isKnown ? erGeo[(size_t)known] : gOwn;
    // the pipe placed from the usual place (a ray from where the listener walked to would end on
    // other walls behind every corner: the pipe and all its reflections would jump), heard from
    // where the listener stands
    const Vec3f L = isKnown ? erListener[(size_t)known] : listenerPos(r),
                S = fixedSrc != nullptr ? *fixedSrc
                                        : roomPoint(placeFrom != nullptr ? *placeFrom : r, srcPos);
    const float rd = seenFrom(L, S).dist;
    const bool topo = s.visShape != r.shape ||
                      s.visN != r.plan.n; // other walls: the old paths mean nothing any more
    const float moved = std::sqrt((S.x - s.visSrc.x) * (S.x - s.visSrc.x) +
                                  (S.y - s.visSrc.y) * (S.y - s.visSrc.y) +
                                  (S.z - s.visSrc.z) * (S.z - s.visSrc.z));
    const bool need = s.fresh || !s.visValid || topo || roomChanged || moved > 0.12f;
    // the searches are spread evenly: every slot may search in one block out of kPlanEvery (new
    // sounds and new plans at once)
    const bool myTurn = ((static_cast<uint32_t>(w.slotIndex) + erBlockCount) %
                         static_cast<uint32_t>(kPlanEvery)) == 0;
    if (need && (s.fresh || topo || !s.visValid || myTurn))
    {
        const int nc = planImageSources(r, g, S, L, r.order, w.paths.data(), kPlanPaths,
                                        w.cands.data(), kPlanCand, kMaxTaps);

        if (topo && !s.fresh)
            for (int i = 0; i < s.numTaps; ++i)
            {
                auto &t = s.taps[(size_t)i];
                if (t.code == -1)
                    continue;
                t.code = -2;
                t.hold = true;
                t.visT = 0.0f; // fade out where it is
            }
        bool keep[kMaxTaps]{};
        int fresh[kMaxTaps], nFresh = 0;
        for (int c = 0; c < nc; ++c)
        {
            const auto &im = w.cands[(size_t)c];
            int found = -1;
            for (int i = 0; i < s.numTaps && found < 0; ++i)
                if (s.taps[(size_t)i].code == im.code)
                    found = i;
            if (found >= 0)
            {
                keep[found] = true;
                s.taps[(size_t)found].visT = 1.0f;
            }
            else
                fresh[nFresh++] = c;
        }
        for (int i = 0; i < s.numTaps; ++i)
        {
            auto &t = s.taps[(size_t)i];
            if (t.code >= 0 && !keep[i])
                t.visT = 0.0f;
        }
        int slot = 0;
        for (int f = 0; f < nFresh; ++f)
        {
            while (slot < kMaxTaps && s.taps[(size_t)slot].code != -1)
                ++slot;
            if (slot >= kMaxTaps)
                break; // full of fading taps: the rest come at the next search
            const auto &im = w.cands[(size_t)fresh[f]];
            auto &t = s.taps[(size_t)slot];
            t.code = im.code;
            t.k = im.k;
            t.w = im.w;
            t.mz = im.mz;
            t.hw = juce::jlimit(0, 3, im.hitsWalls);
            t.hf = juce::jlimit(0, 3, im.hitsFloor);
            t.hold = false;
            t.born = true;
            t.vis = s.fresh ? 1.0f : 0.0f;
            t.visT = 1.0f;
        }
        s.visSrc = S;
        s.visValid = true;
        s.visShape = r.shape;
        s.visN = r.plan.n;
    }

    // every surface with its own absorption (walls, floor and ceiling can be of different stuff):
    // the reflection factor and the loss of highs per hit
    const auto ab = SurfAbs::from(r);
    float Rs[kMaxSurf], Sh[kMaxSurf];
    for (int k = 0; k < kMaxSurf; ++k)
    {
        Rs[k] = std::sqrt(1.0f - ab.low[(size_t)k]);
        Sh[k] = std::sqrt((1.0f - ab.high[(size_t)k]) / juce::jmax(1e-4f, 1.0f - ab.low[(size_t)k]));
    }
    const float spec = std::sqrt(juce::jlimit(0.0f, 1.0f, 1.0f - r.scatter));
    const float airDiff = 0.5f * (airHigh(p.air) - airLow());
    const int encOrder = order == 0 ? 0 : juce::jmin(order, 3);
    const float maxDelay = (float)(kErRing - 4 * kSubBlock);
    const float step = (float)erBlockN / (float)(0.025 * sr);
    const float specPow[4]{1.0f, spec, spec * spec, spec * spec * spec};
    // the walls a path meets (in order), floor and ceiling from its image number
    auto tapLoss = [&](const ErTap &t, float &g, float &sh)
    {
        g = sh = 1.0f;
        for (int j = 0; j < juce::jmin(3, t.k); ++j)
        {
            const int surf = kSurfWall0 + juce::jmin((int)t.w[(size_t)j], kMaxPlanVerts - 1);
            g *= Rs[surf];
            sh *= Sh[surf];
        }
        int lo, hi;
        axisHits(t.mz, lo, hi);
        for (int q = 0; q < lo; ++q)
        {
            g *= Rs[kSurfFloor];
            sh *= Sh[kSurfFloor];
        }
        for (int q = 0; q < hi; ++q)
        {
            g *= Rs[kSurfCeiling];
            sh *= Sh[kSurfCeiling];
        }
        g *= specPow[juce::jlimit(0, 3, t.hw + t.hf)];
    };
    float tmp[kMaxAmbiChannels];
    bool fading = false;
    int last = -1;
    for (int i = 0; i < kMaxTaps; ++i)
    {
        auto &t = s.taps[(size_t)i];
        if (t.code == -1)
        {
            if (i < s.numTaps && (t.g0 > 0.0f || t.g1 > 0.0f))
            {
                t.g1 = 0.0f;
                last = i;
            }
            continue;
        }
        last = i;
        t.vis =
            t.visT > t.vis ? juce::jmin(t.visT, t.vis + step) : juce::jmax(t.visT, t.vis - step);
        // The step above clamps to the exact endpoint; this tests completion.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
#endif
        if (t.vis != t.visT)
            fading = true;
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        float ix = 0.0f, iy = 0.0f;
        if (!t.hold && !planImageOf(g, t.k, t.w, S.x, S.y, ix, iy))
        {
            t.hold = true;
            t.visT = 0.0f;
            fading = true;
        }
        if (t.hold)
        {
            t.g1 = t.gFull * t.vis; // where it was, only fading
        }
        else
        {
            const float iz = imageCoord(t.mz, r.H(), S.z);
            const float dx = ix - L.x, dy = iy - L.y, dz = iz - L.z;
            const float rh = std::sqrt(dx * dx + dy * dy);
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            float delay = (dist - rd) / kSpeedOfSoundRoom * (float)sr;
            float refl, shelf;
            tapLoss(t, refl, shelf);
            float gain = base * distanceGain(dist, distAmountNow) * r.erGain * refl;
            gain *=
                juce::jlimit(0.0f, 1.0f,
                             (maxDelay - delay) /
                                 (0.15f * maxDelay)); // (faded out before the end of the buffer)
            delay = juce::jmin(delay, maxDelay);
            t.d1 = juce::jmax(0.0f, delay);
            t.gFull = gain;
            t.g1 = gain * t.vis;
            t.r1 = shelf * std::exp(-airDiff * dist);
            std::fill(tmp, tmp + kMaxAmbiChannels, 0.0f);
            const int o = (t.hw + t.hf == 1) ? encOrder : juce::jmin(encOrder, 1);
            const double cosAz = rh > 1.0e-6f ? dy / rh : 1.0,
                         sinAz = rh > 1.0e-6f ? -dx / rh : 0.0;
            const double sinEl =
                dist > 1.0e-6f ? juce::jlimit(-1.0, 1.0, (double)(dz / dist)) : 0.0;
            const double cosEl = dist > 1.0e-6f ? (double)(rh / dist) : 1.0;
            if (encOrder == 0)
            {
                encodeStereo(juce::radiansToDegrees(std::atan2(-dx, dy)), tmp);
                t.ch = 2;
            }
            else
            {
                encodeAmbiXDir(cosAz, sinAz, sinEl, cosEl, o, tmp);
                t.ch = (o + 1) * (o + 1);
            }
            std::copy(tmp, tmp + kErCh, t.enc.begin());
        }
        if (t.born || s.fresh)
        {
            t.d0 = t.d1;
            t.r0 = t.r1;
            t.lp = 0.0f;
            t.g0 = s.fresh ? t.g1 : 0.0f;
            t.born = false;
        }
        if (t.vis <= 0.0f && t.visT <= 0.0f)
        {
            t.code = -1;
            t.hold = false;
            t.g1 = 0.0f;
        }
    }
    s.numTaps = last + 1;
    s.fading = fading;
    s.fresh = false;
}

/** the early reflections of one group of slots, into the group's own bus */
void OrganEngine::renderErGroup(int group) noexcept
{
    auto &w = erWork[(size_t)group];
    const EngineParams &p = *erJob.p;
    const int n = erJob.n, K = erJob.K;
    const float inv = erJob.inv, shelfA = erJob.shelfA;
    const bool iceOn = erJob.ice;
    w.active = 0;
    w.used = false;
    const size_t first = (size_t)group * (size_t)kErGroupSlots,
                 last = juce::jmin(erSlots.size(), first + (size_t)kErGroupSlots);
    bool any = false;
    for (size_t si = first; si < last; ++si)
        any = any || erSlots[si].active;
    if (!any)
        return;
    w.used = true;
    float *y = w.y.data();
    std::array<float *, 20> busP{};
    for (int c = 0; c < 20; ++c)
        busP[(size_t)c] = w.bus.data() + (size_t)c * (size_t)n;
    std::fill_n(w.bus.data(), (size_t)(iceOn ? 20 : K) * (size_t)n, 0.0f);
    for (size_t si = first; si < last; ++si)
    {
        auto &s = erSlots[si];
        if (!s.active)
            continue;
        ++w.active;
        w.slotIndex = (int)si;
        if (s.voice >= 0)
        {
            const auto &v = voices[(size_t)s.voice];
            if (v.active && v.erSlot == (int)si)
            {
                const int vr = juce::jlimit(0, kNumRooms - 1, p.stopRoom[(size_t)v.stop]);
                updateSlotTaps(s, w, v.erPos, v.erBase, p, roomsEr[(size_t)vr], &roomsNow[(size_t)vr],
                               v.hasBody ? &v.bodyPos : nullptr);
            }
            else
            {
                s.voice = -1;
                s.drain = kErRing;
            }
        }
        else if (s.noiseVoice >= 0)
        {
            const auto &nv = noise[(size_t)s.noiseVoice];
            if (nv.active && nv.erSlot == (int)si)
                updateSlotTaps(s, w, nv.where, 1.0f, p, roomsEr[0]);
            else
            {
                s.noiseVoice = -1;
                s.drain = kErRing;
            }
        }
        else if (s.input >= 0)
        {
            const auto &is = inSrc[(size_t)s.input];
            if (inRunning && is.erSlot == (int)si)
                updateSlotTaps(s, w, is.pos, 1.0f, p,
                               roomsEr[(size_t)juce::jlimit(0, kNumRooms - 1, p.inputRoom)]);
            else
            {
                s.input = -1;
                s.drain = kErRing;
            }
        }
        else if ((s.drain -= n) <= 0)
        {
            s.active = false;
            continue;
        }

        const float *ring = erRing.data() + si * (size_t)kErRing;
        // the audible taps, sorted by how many channels they are encoded to (first reflections up
        // to 3rd order, later ones at 1st order), then rendered four at a time: four independent
        // filters per sample, and the four results mixed into the channels in one pass
        int hi[kMaxTaps], lo[kMaxTaps], nHi = 0, nLo = 0;
        for (int ti = 0; ti < s.numTaps; ++ti)
        {
            auto &t = s.taps[(size_t)ti];
            if (t.g0 < 3.0e-5f && t.g1 < 3.0e-5f)
            {
                t.d0 = t.d1;
                t.g0 = t.g1;
                t.r0 = t.r1;
                continue;
            } // below -90 dB
            if (t.ch > 4)
                hi[nHi++] = ti;
            else
                lo[nLo++] = ti;
        }
        for (int list = 0; list < 2; ++list)
        {
            const int *idx = list == 0 ? hi : lo;
            const int cnt = list == 0 ? nHi : nLo;
            for (int g0i = 0; g0i < cnt; g0i += 4)
            {
                const int J = juce::jmin(4, cnt - g0i);
                ErTap *tp[4]{nullptr, nullptr, nullptr, nullptr};
                float *xs[4];
                float *ys[4];
                for (int j = 0; j < 4; ++j)
                {
                    xs[j] = y + (size_t)j * (size_t)n;
                    ys[j] = y + (size_t)(4 + j) * (size_t)n;
                }
                int kc = 0;
                for (int j = 0; j < J; ++j)
                {
                    auto &t = s.taps[(size_t)idx[g0i + j]];
                    tp[j] = &t;
                    kc = juce::jmax(kc, juce::jmin(K, t.ch));
                    // the delayed signal (fractional delay ramped over the slice). Integer and
                    // fractional part of the delay kept apart: the clock itself grows without end,
                    // and as a float it would lose the fraction after a few minutes (aliasing, a
                    // high ringing)
                    float *x = xs[j];
                    const float dd = (t.d1 - t.d0) * inv;
                    for (int i = 0; i < n; ++i)
                    {
                        const float d = t.d0 + dd * (float)(i + 1);
                        const int di = (int)d;
                        const float fd = d - (float)di;
                        const uint32_t at = erClock + (uint32_t)i -
                                            (uint32_t)di; // read position + fd samples earlier
                        const float a = ring[(at - 1u) & (kErRing - 1)],
                                    b = ring[at & (kErRing - 1)];
                        x[i] = b + fd * (a - b);
                    }
                }
                for (int j = J; j < 4; ++j)
                    std::fill_n(xs[j], n, 0.0f);
                // high shelf (one-pole low part), four filters side by side
                {
                    const float k1 = 1.0f - shelfA;
                    float l0 = tp[0]->lp, l1 = J > 1 ? tp[1]->lp : 0.0f,
                          l2 = J > 2 ? tp[2]->lp : 0.0f, l3 = J > 3 ? tp[3]->lp : 0.0f;
                    const float *x0 = xs[0];
                    const float *x1 = xs[1];
                    const float *x2 = xs[2];
                    const float *x3 = xs[3];
                    float *y0 = ys[0];
                    float *y1 = ys[1];
                    float *y2 = ys[2];
                    float *y3 = ys[3];
                    for (int i = 0; i < n; ++i)
                    {
                        l0 = l0 * k1 + shelfA * x0[i];
                        y0[i] = l0;
                        l1 = l1 * k1 + shelfA * x1[i];
                        y1[i] = l1;
                        l2 = l2 * k1 + shelfA * x2[i];
                        y2[i] = l2;
                        l3 = l3 * k1 + shelfA * x3[i];
                        y3[i] = l3;
                    }
                    tp[0]->lp = l0;
                    if (J > 1)
                        tp[1]->lp = l1;
                    if (J > 2)
                        tp[2]->lp = l2;
                    if (J > 3)
                        tp[3]->lp = l3;
                }
                // shelf gain and level, ramped
                for (int j = 0; j < J; ++j)
                {
                    auto &t = *tp[j];
                    const float *x = xs[j];
                    float *yy = ys[j];
                    const float dr = (t.r1 - t.r0) * inv, dgn = (t.g1 - t.g0) * inv;
                    for (int i = 0; i < n; ++i)
                    {
                        const float f = (float)(i + 1);
                        const float l = yy[i];
                        yy[i] = (l + (t.r0 + dr * f) * (x[i] - l)) * (t.g0 + dgn * f);
                    }
                }
                for (int j = J; j < 4; ++j)
                    std::fill_n(ys[j], n, 0.0f);
                static const float zeroEnc[kErCh]{};
                const float *e[4];
                for (int j = 0; j < 4; ++j)
                    e[j] = j < J ? tp[j]->enc.data() : zeroEnc;
                if (iceOn)
                    mixGroup4(list == 0 ? busP.data() : busP.data() + 16,
                              list == 0 ? juce::jmin(kc, 16) : juce::jmin(kc, 4), ys, e, n);
                else
                    mixGroup4(busP.data(), kc, ys, e, n);
                for (int j = 0; j < J; ++j)
                {
                    auto &t = *tp[j];
                    t.d0 = t.d1;
                    t.g0 = t.g1;
                    t.r0 = t.r1;
                }
            }
        }    }
}

void OrganEngine::renderRoom(juce::AudioBuffer<float> &out, int n, const EngineParams &p)
{
    const auto &r = p.room;
    roomW = r.W();
    roomL = r.L();
    roomH = r.H();
    roomLx = r.listenerX;
    roomLy = r.listenerY;
    roomAbsW = r.absWalls;
    roomAbsF = r.absFloor;
    roomHighD = r.highDamp;
    roomScat = r.scatter;
    roomAir = p.air;
    roomOutOrder = order;
    roomOn = roomOnNow;
    // which rooms have sources (room 0 always: clicks, effects ...)
    int used = 1;
    for (int st = 0; st < kNumStops; ++st)
        used |= 1 << juce::jlimit(0, kNumRooms - 1, p.stopRoom[(size_t)st]);
    if (p.input.on)
        used |= 1 << juce::jlimit(0, kNumRooms - 1, p.inputRoom);
    roomsUsed = used;
    for (int ri = 0; ri < kNumRooms; ++ri)
    {
        auto &rr = roomsNow[(size_t)ri];
        rr = p.roomAt(ri);
        finishPlan(rr,
                   getDrawnPlan(ri)); // (the shape amount may be modulated: the plan of this block)
        auto &h = heard[(size_t)ri];
        h.W = rr.W();
        h.L = rr.L();
        h.H = rr.H();
        h.Lx = rr.listenerX;
        h.Ly = rr.listenerY;
        {
            // Unmoeglicher Raum: the late reverb of a room of another size; nearer than three
            // metres to a wall it grows (and gets louder) the closer the listener comes
            const auto &lv = p.live;
            float scale = juce::jlimit(0.25f, 8.0f, lv.roomHear), wallGain = 1.0f;
            if (lv.wallPull > 0.0001f)
            {
                const Vec3f lp = listenerPos(rr);
                const float x = lp.x + walkX, y = lp.y + walkY;
                const float d = juce::jmax(0.0f, juce::jmin(juce::jmin(x, rr.W() - x), juce::jmin(y, rr.L() - y)));
                float nearWall = juce::jlimit(0.0f, 1.0f, 1.0f - d / 3.0f);
                nearWall *= nearWall;
                scale *= 1.0f + 3.0f * lv.wallPull * nearWall;
                wallGain = 1.0f + 1.5f * lv.wallPull * nearWall;
            }
            auto &sm = tailScaleSm[(size_t)ri];
            const float k = 1.0f - std::exp(-(float)n / (0.3f * (float)sr));
            sm += k * (scale - sm);
            if (std::abs(sm - scale) < 1.0e-4f)
                sm = scale;
            wallGainNow[(size_t)ri] += k * (wallGain - wallGainNow[(size_t)ri]);
            if (std::abs(wallGainNow[(size_t)ri] - wallGain) < 1.0e-4f)
                wallGainNow[(size_t)ri] = wallGain;
            h.tailScale = sm;
        }
        // the reflections are heard from where the listener has walked to (the late reverb stays
        // that of the usual place)
        roomsEr[(size_t)ri] = rr;
        roomsEr[(size_t)ri].listenerZ = walkZ; // (the reflections are heard from the ears' height)
        if (liveWalk && !rr.box() && rr.plan.n >= 3)
        {
            // a shaped room: the listener walks there and slides along the walls - pushed back onto
            // the nearest wall instead, it would jump at the inner corners (and every reflection
            // with it: a click)
            const auto geo = planGeo(rr);
            const Vec3f home = listenerPos(rr);
            auto &sp = walkSlid[(size_t)ri];
            if (!walkSlidOk[(size_t)ri])
            {
                sp = home;
                walkSlidOk[(size_t)ri] = true;
            } // starts walking: from the usual place
            else if (!planFree(geo, sp.x, sp.y, 0.28f))
                keepInsidePlan(
                    geo, sp.x, sp.y,
                    0.3f); // (the room changed under the listener: a nudge, not a restart)
            planSlide(geo, sp.x, sp.y, home.x + walkX, home.y + walkY, 0.3f,
                      juce::jmax(0.02f, 6.0f * (float)n / (float)sr)); // (at most 6 m/s)
            roomsEr[(size_t)ri].listenerX = juce::jlimit(0.0f, 1.0f, sp.x / rr.W());
            roomsEr[(size_t)ri].listenerY = juce::jlimit(0.0f, 1.0f, sp.y / rr.L());
        }
        else if (liveWalk)
        {
            walkSlidOk[(size_t)ri] = false;
            roomsEr[(size_t)ri].listenerX =
                juce::jlimit(0.02f, 0.98f, rr.listenerX + walkX / rr.W());
            roomsEr[(size_t)ri].listenerY =
                juce::jlimit(0.02f, 0.98f, rr.listenerY + walkY / rr.L());
        }
        else
            walkSlidOk[(size_t)ri] = false;
        erListener[(size_t)ri] = listenerPos(roomsEr[(size_t)ri]);
        if (!roomsEr[(size_t)ri].box())
            erGeo[(size_t)ri] = planGeo(roomsEr[(size_t)ri]);
        h.absW = rr.absWalls;
        h.absF = rr.absFloor;
        h.highD = rr.highDamp;
        h.scat = rr.scatter;
        h.shape = rr.shape;
        h.shapeAmt = rr.shapeAmt;
    }
    ++erBlockCount;
    erBlockN = juce::jmax(1, n);
    if (!roomOnNow || n <= 0)
    {
        erClock += (uint32_t)juce::jmax(0, n);
        for (int ri = 0; ri < kNumRooms; ++ri)
        {
            auto &R = rt[(size_t)ri];
            R.tailGain = 0.0f;
            R.tailWasOn = false;
            resetFdn(R);
            R.convRingOut = R.fdnRingOut = 0;
            R.running = false;
            tailSerialInUseR[(size_t)ri] = R.tail.oldestSerialInUse();
        }
        erActive = 0;
        tailSerialInUse = rt[0].tail.oldestSerialInUse();
        return;
    }
    const int K = juce::jmin(
        usedChannels, order == 0 ? 2 : (juce::jmin(order, 3) + 1) * (juce::jmin(order, 3) + 1));
    const float inv = 1.0f / (float)n;

    // ---- early reflections
    const double profT3 = profOn ? juce::Time::getMillisecondCounterHiRes() : 0.0;
    const float shelfA
 = 1.0f - std::exp(-juce::MathConstants<float>::twoPi * 2500.0f / (float)sr);
    int activeCount = 0;
    // "Eis": the reflections are collected (first ones up to 3rd order, later ones at 1st order),
    // made dispersive, added
    const bool iceOn = p.ice > 1.0e-4f && n <= maxBlock && (int)iceBus.size() >= 20 * n;
    if (iceOn && !iceWasOn)
    {
        iceHi.reset();
        iceLo.reset();
    }
    std::array<float *, 20> busP{};
    if (iceOn)
    {
        for (int c = 0; c < 20; ++c)
            busP[(size_t)c] = iceBus.data() + (size_t)c * (size_t)n;
        std::fill_n(iceBus.data(), (size_t)20 * (size_t)n, 0.0f);
    }
    // (in groups of slots, each into its own bus: AudioWorkers spreads them over the threads)
    {
        int sounding = 0;
        for (const auto &s : erSlots)
            sounding += s.active ? 1 : 0;
        erJob.p = &p;
        erJob.n = n;
        erJob.K = K;
        erJob.inv = inv;
        erJob.shelfA = shelfA;
        erJob.ice = iceOn;
        // (a few sounds are done sooner than the helpers are woken)
        const int want = p.audioThreads > 0 ? p.audioThreads : AudioWorkers::wantedThreads();
        audioWorkers->run(
            kErGroups, [](void *self, int g) { static_cast<OrganEngine *>(self)->renderErGroup(g); },
            this, sounding >= 8 ? want : 1);
        const int busCh = iceOn ? 20 : K;
        float *const *dst = iceOn ? busP.data() : out.getArrayOfWritePointers();
        for (auto &w : erWork)
        {
            if (!w.used)
                continue;
            activeCount += w.active;
            for (int c = 0; c < busCh; ++c)
                juce::FloatVectorOperations::add(dst[c], w.bus.data() + (size_t)c * (size_t)n, n);
            if (profOn)
                prof[5] += w.geoMs;
            w.geoMs = 0.0;
        }
    }
    erActive = activeCount;
    if (profOn)
        prof[3] += juce::Time::getMillisecondCounterHiRes() - profT3;
    ProfScope profTail(profOn, prof[4]);
    if (iceOn)
    {
        // all-pass chains: the later reflections (longer ways) disperse more; 'iceLen' sets how
        // long a chirp is
        const float a = -juce::jmap(juce::jlimit(0.0f, 1.0f, p.iceLen), 0.55f, 0.95f);
        const int Khi = juce::jmin(K, 16), Klo = juce::jmin(K, 4);
        // "Eis" 0..1: from the plain reflections to only the chirps
        const float wet = juce::jlimit(0.0f, 1.0f, p.ice);
        for (int c = 0; c < Khi; ++c)
            juce::FloatVectorOperations::addWithMultiply(out.getWritePointer(c), busP[(size_t)c],
                                                         1.0f - wet, n);
        for (int c = 0; c < Klo; ++c)
            juce::FloatVectorOperations::addWithMultiply(out.getWritePointer(c),
                                                         busP[(size_t)(16 + c)], 1.0f - wet, n);
        iceHi.process(busP.data(), Khi, n, 64, a);
        iceLo.process(busP.data() + 16, Klo, n, 96, a);
        for (int c = 0; c < Khi; ++c)
            juce::FloatVectorOperations::addWithMultiply(out.getWritePointer(c), busP[(size_t)c],
                                                         wet, n);
        for (int c = 0; c < Klo; ++c)
            juce::FloatVectorOperations::addWithMultiply(out.getWritePointer(c),
                                                         busP[(size_t)(16 + c)], wet, n);
    }
    iceWasOn = iceOn;

    // ---- late reverb, one per room in use. The decay comes from the ray tracer (the processor
    // builds the responses in the background); two ways to make the tail: convolution with a
    // synthesised response, or a feedback delay network.
    const int tailCh = juce::jmin(usedChannels, kMaxAmbiChannels);
    std::array<float *, kMaxAmbiChannels> o{};
    for (int c = 0; c < tailCh; ++c)
        o[(size_t)c] = out.getWritePointer(c);
    // room 0 hears everything that is not in one of the other rooms
    {
        float *in0 = room0In.data();
        std::copy_n(roomSend[0].data() + roomOffset, n, in0);
        for (int ri = 1; ri < kNumRooms; ++ri)
            if ((used >> ri) & 1)
                juce::FloatVectorOperations::subtract(in0, rt[(size_t)ri].send.data() + roomOffset,
                                                      n);
    }
    for (int ri = 0; ri < kNumRooms; ++ri)
    {
        auto &R = rt[(size_t)ri];
        auto rm = roomsNow[(size_t)ri];
        {
            // (the late reverb's room: "Unmoeglicher Raum")
            const float ts = tailScaleSm[(size_t)ri];
            if (ts != 1.0f)
            {
                rm.width *= ts;
                rm.length *= ts;
                rm.height *= ts;
            }
        }
        if (!((used >> ri) & 1))
        {
            // not used (any more): rings out with a silent input, then rests
            if (!R.running)
            {
                tailSerialInUseR[(size_t)ri] = R.tail.oldestSerialInUse();
                continue;
            }
        }
        R.running = true;
        const float *in = (used >> ri) & 1 ? (ri == 0 ? room0In.data() : R.send.data() + roomOffset)
                                           : zeroIn.data();
        const TailIR *ir = R.tail.current();
        const float rtL = ir != nullptr ? ir->rtLow : eyringRt(rm, false, p.air);
        const float rtH = ir != nullptr ? ir->rtHigh : eyringRt(rm, true, p.air);
        const float V = roomVolume(rm);
        const float rc = juce::jmax(0.2f, 0.057f * std::sqrt(V / rtL)); // critical distance
        heard[(size_t)ri].rtLow = rtL;
        heard[(size_t)ri].rtHigh = rtH;
        if (ri == 0)
        {
            roomRtLow = rtL;
            roomRtHigh = rtH;
            roomCritDist = rc;
        }
        // reverberant level from the critical distance, softly capped at +4 dB so very long, hard
        // rooms do not drown everything
        const float level = (used >> ri) & 1
                                ? juce::jmin(1.6f, distanceGain(rc, distAmountNow)) * rm.tailGain * wallGainNow[(size_t)ri]
                                : 0.0f;
        const bool useFdn = rm.tailModel == 1;
        if (useFdn != (R.lastTailModel == 1))
        {
            // switched: the other one keeps ringing out with a silent input
            if (useFdn)
                R.convRingOut = TailIR::maxLength(sr);
            else
                R.fdnRingOut = (int)(juce::jmin(30.0f, 1.5f * juce::jmax(rtL, 0.1f)) * (float)sr);
            R.lastTailModel = useFdn ? 1 : 0;
        }
        if (!useFdn || R.convRingOut > 0)
        {
            if (!R.tailWasOn)
            {
                R.tail.reset();
                R.tailWasOn = true;
            }
            const float g0 = R.tailGain,
                        g1 = useFdn ? g0
                                    : g0 + (level - g0) *
                                               (1.0f - std::exp(-(float)n / (0.05f * (float)sr)));
            R.tailGain = g1;
            // the response has unit energy (W)
            R.tail.process(useFdn ? zeroIn.data() : in, o.data(), tailCh, n, g0, g1);
            if (useFdn && (R.convRingOut -= n) <= 0)
            {
                R.convRingOut = 0;
                R.tailWasOn = false;
                R.tailGain = 0.0f;
            }
        }
        if (useFdn && R.convRingOut == 0)
            R.tail.adoptLatest(); // keeps the ray-traced decay current for the FDN
        if (useFdn || R.fdnRingOut > 0)
        {
            // ringing out: the same room level as while it was on (not its own output gain - that
            // would compound)
            if (useFdn)
                R.fdnLevelLast = level;
            renderFdn(R, o.data(), K, useFdn ? in : zeroIn.data(), n, rtL, rtH, R.fdnLevelLast, rm);
            if (!useFdn && (R.fdnRingOut -= n) <= 0)
            {
                R.fdnRingOut = 0;
                resetFdn(R);
            }
        }
        // an unused room stops once its tail has faded
        if (!((used >> ri) & 1) && R.tailGain < 1.0e-4f && R.fdnLevelLast < 1.0e-4f &&
            R.convRingOut == 0 && R.fdnRingOut == 0)
        {
            R.tailGain = 0.0f;
            R.tailWasOn = false;
            resetFdn(R);
            R.running = false;
        }
        tailSerialInUseR[(size_t)ri] = R.tail.oldestSerialInUse();
    }
    tailSerialInUse = rt[0].tail.oldestSerialInUse();
    erClock += (uint32_t)n;
}

void OrganEngine::resetFdn(RoomTail &R) noexcept
{
    R.fdnGain = 0.0f;
    R.fdnPreDelay = -1.0f;
    R.fdnEncOrder = -1; // the next use clears the lines
}

void OrganEngine::renderFdn(RoomTail &R, float *const *o, int K, const float *in, int n, float rtL,
                            float rtH, float gainLevel, const RoomParams &r)
{
    // 16 delay lines (lengths around the mean free path), a random orthogonal mixing matrix,
    // two-band decay per line (ray-traced reverberation times), slow delay modulation, lines spread
    // over 16 directions
    const float mfp = meanFreePath(r);
    rtL = juce::jlimit(0.05f, 30.0f, rtL);
    rtH = juce::jlimit(0.05f, 30.0f, juce::jmin(rtH, rtL));
    const float inv = 1.0f / (float)n;
    static const float ratios[kFdnLines]{0.53f, 0.59f, 0.67f, 0.71f, 0.77f, 0.83f, 0.89f, 0.97f,
                                         1.03f, 1.09f, 1.13f, 1.21f, 1.27f, 1.33f, 1.41f, 1.49f};
    const float k = 1.0f - std::exp(-(float)n / (0.08f * (float)sr));
    std::array<float, kFdnLines> dOld = R.fdnDelay, dNew{};
    const bool first = R.fdnPreDelay < 0.0f;
    if (first)
    {
        std::fill(R.fdnBuf.begin(), R.fdnBuf.end(), 0.0f);
        std::fill(R.fdnPre.begin(), R.fdnPre.end(), 0.0f);
        R.fdnLp.fill(0.0f);
    }
    float gMean = 0.0f;
    for (int j = 0; j < kFdnLines; ++j)
    {
        const float target = juce::jlimit(64.0f, (float)(kFdnLen - 4 * kSubBlock),
                                          mfp / kSpeedOfSoundRoom * ratios[j] * (float)sr);
        if (first)
            dOld[(size_t)j] = target;
        dNew[(size_t)j] = dOld[(size_t)j] + (target - dOld[(size_t)j]) * k;
        R.fdnGL[(size_t)j] = std::pow(10.0f, -3.0f * dNew[(size_t)j] / ((float)sr * rtL));
        R.fdnGH[(size_t)j] = std::pow(10.0f, -3.0f * dNew[(size_t)j] / ((float)sr * rtH));
        gMean += R.fdnGL[(size_t)j] / (float)kFdnLines;
    }
    R.fdnDelay = dNew;
    const float preTarget = juce::jlimit(0.0f, (float)(kFdnLen - 4 * kSubBlock),
                                         0.5f * mfp / kSpeedOfSoundRoom * (float)sr);
    const float pre0 = first ? preTarget : R.fdnPreDelay;
    const float pre1 = pre0 + (preTarget - pre0) * k;
    R.fdnPreDelay = pre1;
    const float gainTarget =
        std::sqrt(juce::jmax(1.0e-6f, 1.0f - gMean * gMean)) * kFdnLevel * gainLevel;
    const float gain0 = first ? 0.0f : R.fdnGain, gain1 = gain0 + (gainTarget - gain0) * k;
    R.fdnGain = gain1;
    const int encOrder = order == 0 ? 0 : juce::jmin(order, 3);
    if (encOrder != R.fdnEncOrder)
    {
        R.fdnEncOrder = encOrder;
        for (int j = 0; j < kFdnLines; ++j)
        {
            const float zz = 1.0f - 2.0f * ((float)j + 0.5f) / (float)kFdnLines;
            const float az = std::fmod(137.508f * (float)j, 360.0f) - 180.0f;
            const float el = juce::radiansToDegrees(std::asin(zz));
            auto &e = R.fdnEnc[(size_t)j];
            std::fill(e.begin(), e.end(), 0.0f);
            if (encOrder == 0)
                encodeStereo(az, e.data());
            else
                encodeAmbiX(az, el, encOrder, e.data());
        }
    }
    const float splitA = 1.0f - std::exp(-juce::MathConstants<float>::twoPi * 1500.0f / (float)sr);
    constexpr uint32_t mask = kFdnLen - 1;
    std::array<float, kFdnLines> yl{};
    const float modDepth = (float)(8.0 * sr / 48000.0);
    // read position: the integer clock and the fractional delay are kept apart (a float clock would
    // lose the fraction after a few minutes - aliasing, a high ringing)
    auto readLine = [mask](const float *line, uint32_t now, float d)
    {
        const int di = (int)d;
        const float fd = d - (float)di;
        const uint32_t at = now - (uint32_t)di;
        const float b = line[at & mask], a = line[(at - 1u) & mask];
        return b + fd * (a - b);
    };
    for (int i = 0; i < n; ++i)
    {
        const float f = (float)(i + 1) * inv;
        R.fdnPre[R.fdnPos & mask] = in[i];
        const float x = readLine(R.fdnPre.data(), R.fdnPos, pre0 + (pre1 - pre0) * f);
        for (int j = 0; j < kFdnLines; ++j)
        {
            // slow modulation of the delay (+-8 samples at 48 kHz): no fixed modes that ring
            const float c = R.fdnModC[(size_t)j], sn = R.fdnModS[(size_t)j];
            R.fdnModC[(size_t)j] = c * fdnModStepC[(size_t)j] - sn * fdnModStepS[(size_t)j];
            R.fdnModS[(size_t)j] = sn * fdnModStepC[(size_t)j] + c * fdnModStepS[(size_t)j];
            const float d =
                dOld[(size_t)j] + (dNew[(size_t)j] - dOld[(size_t)j]) * f + modDepth * sn;
            const float v = readLine(R.fdnBuf.data() + (size_t)j * (size_t)kFdnLen, R.fdnPos, d);
            R.fdnLp[(size_t)j] += splitA * (v - R.fdnLp[(size_t)j]);
            yl[(size_t)j] = R.fdnGL[(size_t)j] * R.fdnLp[(size_t)j] +
                            R.fdnGH[(size_t)j] * (v - R.fdnLp[(size_t)j]);
        }
        for (int j = 0; j < kFdnLines; ++j)
        {
            const auto &row = fdnMatrix[(size_t)j];
            float h = 0.0f;
            for (int k2 = 0; k2 < kFdnLines; ++k2)
                h += row[(size_t)k2] * yl[(size_t)k2];
            R.fdnBuf[(size_t)j * (size_t)kFdnLen + (R.fdnPos & mask)] =
                h + ((j & 1) ? -0.25f : 0.25f) * x;
        }
        const float g = gain0 + (gain1 - gain0) * f;
        for (int c = 0; c < K; ++c)
        {
            float s = 0.0f;
            for (int j = 0; j < kFdnLines; ++j)
                s += yl[(size_t)j] * R.fdnEnc[(size_t)j][(size_t)c];
            o[c][i] += s * g;
        }
        ++R.fdnPos;
    }
    for (int j = 0; j < kFdnLines; ++j) // keep the modulation phasors on the unit circle
    {
        const float len = std::sqrt(R.fdnModC[(size_t)j] * R.fdnModC[(size_t)j] +
                                    R.fdnModS[(size_t)j] * R.fdnModS[(size_t)j]);
        if (len > 0.0f)
        {
            R.fdnModC[(size_t)j] /= len;
            R.fdnModS[(size_t)j] /= len;
        }
    }
}

// ---------------------------------------------------------------------------
// wind leaks and wandering echoes
// ---------------------------------------------------------------------------
void OrganEngine::renderLeaks(juce::AudioBuffer<float> &out, int n, const EngineParams &p)
{
    const auto &fx = p.fx;
    const int count = juce::jlimit(1, (int)leaks.size(), fx.leakCount);
    if (fx.leakLevel <= 0.0001f)
    {
        for (auto &L : leaks)
        {
            L.amp = 0.0f;
            L.s1 = L.s2 = 0.0f;
            L.g1.fill(0.0f);
        }
        return;
    }
    const float inv = 1.0f / (float)n;
    float *buf = fxScratch.data();
    float *ramp = fxScratch.data() + maxBlock;
    const float ampK = 1.0f - std::exp(-1.0f / (0.02f * (float)sr)); // 20 ms smoothing
    for (int li = 0; li < (int)leaks.size(); ++li)
    {
        auto &L = leaks[(size_t)li];
        if (li >= count)
        {
            L.amp = 0.0f;
            L.g1.fill(0.0f);
            continue;
        }
        L.pos.az =
            wrapAz(L.pos.az + L.dir * fx.leakWander * chunkSeconds * (0.6f + 0.4f * L.freqMul));
        L.g0 = L.g1;
        gainsFor(L.pos, L.g1.data());
        const float dg = distanceGain(L.pos.dist, distAmountNow);
        for (auto &x : L.g1)
            x *= dg;

        const float fc = juce::jmin(0.45f * (float)sr, fx.leakFreq * L.freqMul);
        const float g = std::tan(juce::MathConstants<float>::pi * fc / (float)sr);
        const float k = 1.0f / 3.0f; // Q 3
        const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const float lvl = 0.9f * fx.leakLevel;
        for (int i = 0; i < n; ++i)
        {
            if (--L.ampCount <= 0)
            {
                // a leak hisses unevenly: it swells, flutters, sometimes stops for a moment
                const float u = L.rng.uni();
                L.ampTarget = u < 0.12f ? 0.0f : 0.25f + 0.75f * L.rng.uni();
                L.ampCount = (int)((0.04f + 0.7f * L.rng.uni() * L.rng.uni()) * (float)sr);
            }
            L.amp += ampK * (L.ampTarget - L.amp);
            const float x = L.rng.bi();
            const float v3 = x - L.s2;
            const float v1 = a1 * L.s1 + a2 * v3;
            const float v2 = L.s2 + a2 * L.s1 + a3 * v3;
            L.s1 = 2.0f * v1 - L.s1;
            L.s2 = 2.0f * v2 - L.s2;
            buf[i] = v1 * k * L.amp * lvl;
            ramp[i] = buf[i] * (float)i;
        }
        {
            float d[kMaxAmbiChannels];
            for (int c = 0; c < usedChannels; ++c)
                d[c] = (L.g1[(size_t)c] - L.g0[(size_t)c]) * inv;
            mixInto(out.getArrayOfWritePointers(), 0, usedChannels, buf, ramp, L.g0.data(), d, n);
        }
        // a little of it into the room, from its own direction
        const double az =
            juce::degreesToRadians((double)(roomFollowNow ? L.pos.az : roomBaseAzNow));
        for (int kk = 0; kk < roomSends; ++kk)
        {
            const int m = (kk + 1) / 2;
            const float w =
                kk == 0 ? 1.0f : (float)(kk % 2 == 1 ? std::cos(m * az) : std::sin(m * az));
            juce::FloatVectorOperations::addWithMultiply(roomSend[(size_t)kk].data() + roomOffset,
                                                         buf, 0.3f * w, n);
        }
    }
}

void OrganEngine::renderEchoes(juce::AudioBuffer<float> &out, int n, const EngineParams &p)
{
    const auto &fx = p.fx;
    const int len = (int)echoBuf.size();
    if (len < 4 * n || n <= 0)
        return;
    // input: the whole dry organ (W, or the middle of the stereo pair)
    const float *in0 = out.getReadPointer(0);
    const float *in1 = usedChannels > 1 ? out.getReadPointer(1) : in0;
    for (int i = 0; i < n; ++i)
    {
        const float x = order == 0 ? 0.5f * (in0[i] + in1[i]) : in0[i];
        echoBuf[(size_t)((echoWrite + i) % len)] = x;
    }
    if (fx.echoLevel > 0.0001f)
    {
        const int repeats = juce::jlimit(1, 8, fx.echoRepeats);
        double target = (fx.echoSync ? lfoDivQuarters(fx.echoDiv) * 60.0 / juce::jmax(20.0, p.bpm)
                                     : (double)fx.echoTime) *
                        sr;
        target = juce::jlimit((double)n + 4.0, (double)(len - n - 8) / repeats, target);
        if (echoDelay < 0.0)
            echoDelay = target;
        const double d0 = echoDelay;
        const double d1 = d0 + (target - d0) * (1.0 - std::exp(-(double)chunkSeconds / 0.12));
        echoDelay = d1;
        float *y = fxScratch.data();
        std::array<float, kMaxAmbiChannels> tg{};
        for (int r = 1; r <= repeats; ++r)
        {
            const float gain = fx.echoLevel * std::pow(fx.echoFeedback, (float)(r - 1));
            if (gain < 1.0e-4f)
                break;
            const Pos dir{wrapAz(fx.echoTurn * (float)r),
                          juce::jlimit(-80.0f, 80.0f, fx.echoTilt * (float)r), kRefDist};
            gainsFor(dir, tg.data());
            const float fc =
                juce::jmax(250.0f, 18000.0f * std::pow(1.0f - 0.85f * fx.echoDamp, (float)r));
            const float a =
                fc >= 17000.0f
                    ? 1.0f
                    : 1.0f - std::exp(-juce::MathConstants<float>::twoPi * fc / (float)sr);
            float &lp = echoLp[(size_t)(r - 1)];
            for (int i = 0; i < n; ++i)
            {
                const double dd = (d0 + (d1 - d0) * (double)i / (double)n) * r;
                double rp = (double)(echoWrite + i) - dd;
                while (rp < 0.0)
                    rp += len;
                const int i0 = (int)rp;
                const float f = (float)(rp - i0);
                const float x0 = echoBuf[(size_t)(i0 % len)],
                            x1 = echoBuf[(size_t)((i0 + 1) % len)];
                lp += a * (x0 + f * (x1 - x0) - lp);
                y[i] = lp * gain;
            }
            mixInto(out.getArrayOfWritePointers(), 0, usedChannels, y, nullptr, tg.data(), nullptr,
                    n);
            const double az =
                juce::degreesToRadians((double)(roomFollowNow ? dir.az : roomBaseAzNow));
            for (int kk = 0; kk < roomSends; ++kk)
            {
                const int m = (kk + 1) / 2;
                const float w =
                    kk == 0 ? 1.0f : (float)(kk % 2 == 1 ? std::cos(m * az) : std::sin(m * az));
                juce::FloatVectorOperations::addWithMultiply(
                    roomSend[(size_t)kk].data() + roomOffset, y, 0.35f * w, n);
            }
        }
    }
    else
        echoDelay = -1.0;
    echoWrite = (echoWrite + n) % len;
}

// ---------------------------------------------------------------------------
// modulation
// ---------------------------------------------------------------------------
void OrganEngine::advanceModulation(const EngineParams &p, double dt) noexcept
{
    const ModParams &m =
        haveLastMod ? lastMod : p.mod; // last slice's (possibly modulated) settings
    for (int i = 0; i < kNumLfos; ++i)
    {
        const auto &L = m.lfo[(size_t)i];
        const double q = lfoDivQuarters(L.div);
        double ph = lfoPhase[(size_t)i];
        if (L.sync && p.playing)
            ph = p.ppq / q;
        else
            ph += (L.sync ? p.bpm / 60.0 / q : (double)L.rate) * dt;
        if (std::abs(ph) > 1.0e6)
            ph = std::fmod(ph, 1000.0);
        lfoPhase[(size_t)i] = ph;
        lfoPhaseOut[(size_t)i] = ph;
        globalSrc[(size_t)(srcLfo1 + i)] = lfoValue(L.shape, ph, 0);
    }
    bool any = false;
    for (int k = 0; k < kNumKeys; ++k)
        any = any || keyHeld[(size_t)k] || keySustained[(size_t)k];
    for (int j = 0; j < kNumEnvs; ++j)
    {
        auto &e = globalEnv[(size_t)j];
        if (globalTrig)
            e.trigger();
        if (!any && e.gate)
            e.releaseGate();
        e.advance(m.env[(size_t)j], (float)dt);
        globalSrc[(size_t)(srcEnv1 + j)] = e.level;
    }
    globalTrig = false;
    globalSrc[srcKey] = lastKeyPos;
    globalSrc[srcModWheel] = modWheel;
    globalSrc[srcRandom] = lastRandom;
    globalSrc[srcVelocity] = lastVelocity;
    globalSrc[srcPitchWheel] = pitchWheel;
    globalSrc[srcPressure] = lastPress;
    globalSrc[srcSlide] = lastSlide;
    globalSrc[srcInput] = inFollowVal;
    globalSrc[srcBeat] = juce::jlimit(0.0f, 1.0f, std::log2(1.0f + beatMaxNow.load()) / 5.044f);
    globalSrc[srcBellows] = juce::jlimit(0.0f, 1.0f, bellowsP);
    for (int i = 0; i < kNumSources; ++i)
        sourceOut[(size_t)i] = globalSrc[(size_t)i];
}

static inline float lfoSpreadOffset(const LfoParams &L, int stop, int key) noexcept
{
    if (L.spread <= 0.0f)
        return 0.0f;
    return L.spread * (L.spreadMode == 0 ? (float)key / (float)kNumKeys
                                         : hash01((uint32_t)(stop * 131 + key * 7 + 1)));
}

Pos OrganEngine::displayPosition(int stop, int key, const EngineParams &p) const noexcept
{
    Pos pos = currentPosition(stop, key, p);
    float az = 0.0f, el = 0.0f, dm = 1.0f;
    if ((soundingMask[(size_t)stop].load() >> key) & 1)
    {
        az = modAz[(size_t)stop][(size_t)key].load();
        el = modEl[(size_t)stop][(size_t)key].load();
        dm = modDistMul[(size_t)stop][(size_t)key].load();
    }
    else
    {
        std::array<float, kNumPipeTargets> pm{};
        bool any = false;
        for (const auto &r : p.mod.routes)
        {
            if (r.src < 0 || r.target < 0 || r.target > tgtDist || r.amount == 0.0f)
                continue;
            float v = 0.0f;
            if (r.src >= srcLfo1 && r.src < srcLfo1 + kNumLfos)
            {
                const auto &L = p.mod.lfo[(size_t)(r.src - srcLfo1)];
                v = lfoValue(L.shape,
                             lfoPhaseOut[(size_t)(r.src - srcLfo1)].load() +
                                 lfoSpreadOffset(L, stop, key),
                             0);
            }
            else if (r.src >= srcEnv1 && r.src < srcEnv1 + kNumEnvs)
                v = p.mod.env[(size_t)(r.src - srcEnv1)].perPipe ? 0.0f
                                                                 : sourceOut[(size_t)r.src].load();
            else if (r.src == srcKey)
                v = (float)key / (float)(kNumKeys - 1) * 2.0f - 1.0f;
            else if (r.src == srcModWheel || r.src == srcPitchWheel || r.src == srcVelocity ||
                     r.src == srcPressure || r.src == srcSlide || r.src == srcInput ||
                     r.src == srcBellows)
                v = sourceOut[(size_t)r.src].load();
            pm[(size_t)r.target] += r.amount * shapeSource(r.src, v, r.uni);
            any = true;
        }
        if (!any)
            return pos;
        az = 180.0f * pm[tgtAz];
        el = 90.0f * pm[tgtEl];
        dm = std::exp2(3.0f * juce::jlimit(-2.0f, 2.0f, pm[tgtDist]));
    }
    pos.az = wrapAz(pos.az + az);
    pos.el = juce::jlimit(-90.0f, 90.0f, pos.el + el);
    pos.dist = juce::jlimit(kMinDist, kMaxDist, pos.dist * dm);
    return pos;
}

float OrganEngine::voicePressure(const Voice &v) const noexcept
{
    // MPE: the note's channel; otherwise channel pressure (any channel) - polyphonic aftertouch
    // wins when higher
    const float poly = v.noteId >= 0 ? notePress[(size_t)v.noteId] : keyPress[(size_t)v.key];
    const float chan =
        mpeNow && v.noteId >= 128 ? chPress[(size_t)(v.noteId >> 7)] : lastPressChannel();
    return juce::jmax(poly, chan);
}

float OrganEngine::voiceSlide(const Voice &v) const noexcept
{
    return mpeNow && v.noteId >= 128 ? chSlide[(size_t)(v.noteId >> 7)] : lastSlide;
}

float OrganEngine::voiceSource(const Voice &v, int src, const EngineParams &p) const noexcept
{
    if (src >= srcLfo1 && src < srcLfo1 + kNumLfos)
    {
        const int i = src - srcLfo1;
        const auto &L = p.mod.lfo[(size_t)i];
        return lfoValue(L.shape, lfoPhase[(size_t)i] + lfoSpreadOffset(L, v.stop, v.key), 0);
    }
    if (src >= srcEnv1 && src < srcEnv1 + kNumEnvs)
    {
        const int j = src - srcEnv1;
        return p.mod.env[(size_t)j].perPipe ? v.menv[(size_t)j].level : globalEnv[(size_t)j].level;
    }
    switch (src)
    {
    case srcKey:
        return v.keyPos;
    case srcModWheel:
        return modWheel;
    case srcRandom:
        return v.randMod;
    case srcVelocity:
        return v.velMod;
    case srcPitchWheel:
        return pitchWheel;
    case srcPressure:
        return voicePressure(v);
    case srcSlide:
        return voiceSlide(v);
    case srcInput:
        return inFollowVal;
    case srcBeat:
        return v.beatNorm;
    case srcBellows:
        return juce::jlimit(0.0f, 1.0f, bellowsP);
    default:
        return 0.0f;
    }
}

// ---------------------------------------------------------------------------
// "Raumverfall" and "Körnerwolke"
// ---------------------------------------------------------------------------
void OrganEngine::updateGrid(float amount) noexcept
{
    const int n = gridCount(amount);
    if (n == gridN)
        return;
    if (n > 0)
    {
        fibonacciDirs(n, gridAz.data(), gridEl.data());
        for (int i = 0; i < n; ++i)
        {
            const float a = juce::degreesToRadians(gridAz[(size_t)i]),
                        e = juce::degreesToRadians(gridEl[(size_t)i]);
            gridX[(size_t)i] = -std::sin(a) * std::cos(e);
            gridY[(size_t)i] = std::cos(a) * std::cos(e);
            gridZ[(size_t)i] = std::sin(e);
        }
    }
    gridN = n;
}

/** the pipe's sound into its history ring; new grains are born from it */
void OrganEngine::cloudWrite(Voice &v, const float *buf, const float *bufH, int n,
                             const EngineParams &p) noexcept
{
    if (ringSize <= 0)
        return;
    const auto cl = cloudFor(p.cloud[(size_t)v.stop], v);
    if (v.cloudRing < 0)
    {
        if (!cl.on())
            return;
        for (int r = 0; r < kCloudRings; ++r)
            if (ringOwner[(size_t)r] == 0 && ringRefs[(size_t)r] == 0)
            {
                v.cloudRing = r;
                ringOwner[(size_t)r] = v.age;
                ringPos[(size_t)r] = 0;
                std::fill_n(cloudData.data() + (size_t)r * (size_t)ringSize, ringSize, 0.0f);
                break;
            }
        if (v.cloudRing < 0)
            return; // all rings in use: this pipe stays whole
    }
    const int r = v.cloudRing;
    float *ring = cloudData.data() + (size_t)r * (size_t)ringSize;
    const uint32_t w = ringPos[(size_t)r];
    for (int i = 0; i < n; ++i)
        ring[(w + (uint32_t)i) & ringMask] = buf[i] + (bufH != nullptr ? bufH[i] : 0.0f);
    ringPos[(size_t)r] = w + (uint32_t)n;
    if (!cl.on())
        return;

    // new grains: on average 'density' per second, each from a little earlier in the pipe's sound,
    // somewhere on the sphere around the pipe
    v.cloudAcc += (double)cl.density * n / sr * (0.5 + cloudRng.uni());
    const float sizeS = juce::jmax(0.005f, cl.sizeMs * 0.001f);
    const float wet =
        std::sin(0.5f * juce::MathConstants<float>::pi * juce::jlimit(0.0f, 1.0f, cl.mix));
    // overlapping grains add up in energy; a Hann window keeps 3/8 of it
    const float amp = wet * 1.633f / std::sqrt(juce::jmax(1.0f, cl.density * sizeS));
    const int maxDelay = ringSize - maxBlock - 4;
    while (v.cloudAcc >= 1.0)
    {
        v.cloudAcc -= 1.0;
        int gi = -1;
        for (int k = 0; k < kCloudGrains; ++k)
            if (!grains[(size_t)k].on)
            {
                gi = k;
                break;
            }
        if (gi < 0)
        {
            v.cloudAcc = 0.0;
            break;
        } // the pool is full
        auto &g = grains[(size_t)gi];
        // direction: uniform in the cap of 'spread' degrees around the pipe
        const float a0 = juce::degreesToRadians(v.spPos.az),
                    e0 = juce::degreesToRadians(v.spPos.el);
        const float ux = -std::sin(a0) * std::cos(e0), uy = std::cos(a0) * std::cos(e0),
                    uz = std::sin(e0);
        float t1x = -uy, t1y = ux, t1z = 0.0f; // a tangent (horizontal unless at the pole)
        float tl = std::sqrt(t1x * t1x + t1y * t1y);
        if (tl < 1.0e-4f)
        {
            t1x = 1.0f;
            t1y = 0.0f;
            tl = 1.0f;
        }
        t1x /= tl;
        t1y /= tl;
        const float t2x = uy * t1z - uz * t1y, t2y = uz * t1x - ux * t1z, t2z = ux * t1y - uy * t1x;
        const float cosMax =
            std::cos(juce::degreesToRadians(juce::jlimit(0.0f, 180.0f, cl.spread)));
        const float ct = 1.0f - cloudRng.uni() * (1.0f - cosMax),
                    st = std::sqrt(juce::jmax(0.0f, 1.0f - ct * ct));
        const float ph = juce::MathConstants<float>::twoPi * cloudRng.uni();
        const float dx = ux * ct + st * (std::cos(ph) * t1x + std::sin(ph) * t2x);
        const float dy = uy * ct + st * (std::cos(ph) * t1y + std::sin(ph) * t2y);
        const float dz = uz * ct + st * (std::cos(ph) * t1z + std::sin(ph) * t2z);
        Pos gp;
        gp.az = juce::radiansToDegrees(std::atan2(-dx, dy));
        gp.el = juce::radiansToDegrees(std::asin(juce::jlimit(-1.0f, 1.0f, dz)));
        gp.dist = v.spPos.dist;
        gainsFor(gp, g.g.data());
        const float k = v.spK * amp;
        for (auto &x : g.g)
            x *= k;
        const int delay = juce::jlimit(
            1, maxDelay, 1 + (int)(cloudRng.uni() * cl.scatterMs * 0.001f * (float)sr));
        g.on = true;
        g.dying = false;
        g.fade = 1.0f;
        g.ring = r;
        g.owner = v.age;
        g.len = juce::jmax(32, (int)(sizeS * (0.7f + 0.6f * cloudRng.uni()) * (float)sr));
        g.age = 0;
        g.rp = ringPos[(size_t)r] - (uint32_t)n -
               (uint32_t)delay; // this block's first sample, 'delay' earlier
        ++ringRefs[(size_t)r];
    }
}

void OrganEngine::renderCloud(float *const *outs, int start, int n) noexcept
{
    if (ringSize <= 0)
        return;
    // rings whose pipe has gone: no owner any more (freed when no grain reads them)
    for (int r = 0; r < kCloudRings; ++r)
    {
        const uint32_t own = ringOwner[(size_t)r];
        if (own == 0)
            continue;
        bool alive = false;
        for (const auto &v : voices)
            if (v.active && v.age == own && v.cloudRing == r)
            {
                alive = true;
                break;
            }
        if (!alive)
            ringOwner[(size_t)r] = 0;
    }
    float *y = fxScratch.data() + 2 * (size_t)maxBlock;
    const float fadeStep = (float)(1.0 / (0.005 * sr));
    const float twoPi = juce::MathConstants<float>::twoPi;
    int count = 0;
    for (auto &g : grains)
    {
        if (!g.on)
            continue;
        ++count;
        if (!g.dying && ringOwner[(size_t)g.ring] != g.owner)
            g.dying = true; // the pipe is gone: fade out quickly
        const float *ring = cloudData.data() + (size_t)g.ring * (size_t)ringSize;
        const float inc = 1.0f / (float)g.len;
        int m = 0;
        bool ended = false;
        for (; m < n; ++m)
        {
            if (g.age >= g.len)
            {
                ended = true;
                break;
            }
            float w = 0.5f - 0.5f * std::cos(twoPi * (float)g.age * inc);
            if (g.dying)
            {
                g.fade -= fadeStep;
                if (g.fade <= 0.0f)
                {
                    ended = true;
                    break;
                }
                w *= g.fade;
            }
            y[m] = ring[(g.rp + (uint32_t)m) & ringMask] * w;
            ++g.age;
        }
        for (int i = m; i < n; ++i)
            y[i] = 0.0f;
        g.rp += (uint32_t)n;
        if (m > 0)
            mixInto(outs, start, usedChannels, y, nullptr, g.g.data(), nullptr, m);
        if (ended)
        {
            g.on = false;
            --ringRefs[(size_t)g.ring];
        }
    }
    cloudGrainsActive = count;
}

void OrganEngine::process(juce::AudioBuffer<float> &out, const juce::MidiBuffer &midi,
                          const EngineParams &pIn)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = out.getNumSamples();
    if (n <= 0)
        return;
    for (auto &r : roomSend)
        if ((int)r.size() < n)
            r.assign((size_t)n, 0.0f); // safety (prepare() sizes them)

    // render in short slices: every slice gets fresh positions, so even 10 Hz motion stays smooth
    for (int start = 0; start < n; start += kSubBlock)
    {
        const int len = juce::jmin(kSubBlock, n - start);
        juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), out.getNumChannels(), start,
                                      len);
        chunkMidi.clear();
        chunkMidi.addEvents(midi, start, len, -start);
        EngineParams pc = pIn;
        pc.ppq = pIn.ppq + (start / sr) * pIn.bpm / 60.0;
        followInput(start, len, pIn);
        advanceModulation(pc, len / sr);
        if (modHook != nullptr)
            modHook->modulate(pc, globalSrc.data());
        lastMod = pc.mod;
        haveLastMod = true;
        roomOffset = start;
        processChunk(view, chunkMidi, pc);
    }
    roomOffset = 0;
    renderLucier(out, n, pIn);

    // meters
    const float pk0 = out.getNumChannels() > 0 ? out.getMagnitude(0, 0, n) : 0.0f;
    const float pk1 =
        out.getNumChannels() > 1 ? (order == 0 ? out.getMagnitude(1, 0, n) : pk0) : 0.0f;
    meterL = juce::jmax(pk0, meterL.load() * 0.9f);
    meterR = juce::jmax(pk1, meterR.load() * 0.9f);
}

// ---------------------------------------------------------------------------
// Lucier mode: the output (W / L+R) through the room n times, added back as one (omni / centre)
// signal
// ---------------------------------------------------------------------------
void OrganEngine::renderLucier(juce::AudioBuffer<float> &out, int n, const EngineParams &p) noexcept
{
    const bool on = p.lucierOn && lucier.current() != nullptr;
    if (!on && !lucRunning)
    {
        lucierSerialInUse = lucier.oldestSerialInUse();
        lucierLevel = 0.0f;
        return;
    }
    if (on && !lucRunning)
    {
        lucier.reset();
        lucRunning = true;
        lucInEnv = lucOutEnv = 0.0f;
        lucAgc = 1.0f;
    }
    if (on)
        lucRing = TailIR::maxLength(sr);
    const int chans = out.getNumChannels();
    if (chans <= 0 || n > (int)lucIn.size())
        return;
    const bool stereo = order == 0 && chans > 1;
    // input: what the listener hears (omni)
    const float *a = out.getReadPointer(0);
    if (on)
    {
        if (stereo)
        {
            const float *b = out.getReadPointer(1);
            for (int i = 0; i < n; ++i)
                lucIn[(size_t)i] = 0.5f * (a[i] + b[i]);
        }
        else
            std::copy_n(a, n, lucIn.data());
    }
    else
        std::fill_n(lucIn.data(), n, 0.0f); // switched off: the room rings out
    std::fill_n(lucWet.data(), n, 0.0f);
    // dry part fades with the mix (equal power), the wet part comes in; the response has unit-ish
    // energy
    const float mix = juce::jlimit(0.0f, 1.0f, p.lucierMix);
    const float dryT = on ? std::cos(mix * juce::MathConstants<float>::halfPi) : 1.0f;
    const float wetT = std::sin(mix * juce::MathConstants<float>::halfPi);
    const float k = 1.0f - std::exp(-(float)n / (0.05f * (float)sr));
    const float d0 = lucDry, d1 = lucDry + (dryT - lucDry) * k;
    const float w0 = lucWetGain, w1 = lucWetGain + (wetT - lucWetGain) * k;
    lucDry = d1;
    lucWetGain = w1;
    float *wp = lucWet.data();
    lucier.process(lucIn.data(), &wp, 1, n, 1.0f, 1.0f);
    // level: the passes keep only narrow resonances - an organ's partials may hit them or not. Like
    // Lucier's re-recording (each generation brought back up to level), the result is kept about as
    // loud as what went in; in silence the gain holds, so the room rings out naturally
    {
        double ei = 0.0, eo = 0.0;
        for (int i = 0; i < n; ++i)
        {
            ei += (double)lucIn[(size_t)i] * lucIn[(size_t)i];
            eo += (double)lucWet[(size_t)i] * lucWet[(size_t)i];
        }
        auto coef = [&](float seconds)
        { return 1.0f - std::exp(-(float)n / (seconds * (float)sr)); };
        const float pi = (float)(ei / n), po = (float)(eo / n);
        lucInEnv += coef(0.4f) * (pi - lucInEnv);
        lucOutEnv += (po > lucOutEnv ? coef(0.03f) : coef(0.4f)) *
                     (po - lucOutEnv); // a resonance that is hit: noticed at once
        if (on && lucInEnv > 1.0e-9f && lucOutEnv > 1.0e-14f)
        {
            const float target = juce::jlimit(0.25f, 100.0f, std::sqrt(lucInEnv / lucOutEnv));
            // down quickly (no bursts when a chord hits a resonance), up slowly
            lucAgc *= std::pow(target / lucAgc, target < lucAgc ? coef(0.03f) : coef(1.5f));
        }
        const float g0 = w0 * lucAgc, g1 = w1 * lucAgc;
        const float inv0 = 1.0f / (float)n;
        for (int i = 0; i < n; ++i)
            lucWet[(size_t)i] *= g0 + (g1 - g0) * (float)(i + 1) * inv0;
    }
    // soft limit: the strongest resonance of a high generation can be very narrow and loud
    float pk = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float x = lucWet[(size_t)i];
        lucWet[(size_t)i] = x / std::sqrt(1.0f + 0.25f * x * x);
        pk = juce::jmax(pk, std::abs(lucWet[(size_t)i]));
    }
    lucierLevel = juce::jmax(pk, lucierLevel.load() * 0.9f);
    const float inv = 1.0f / (float)n;
    for (int c = 0; c < chans; ++c)
    {
        float *o = out.getWritePointer(c);
        const bool takes = stereo ? c < 2 : c == 0;
        const float wg = stereo ? 0.7071f : 1.0f;
        for (int i = 0; i < n; ++i)
        {
            const float dg = d0 + (d1 - d0) * (float)(i + 1) * inv;
            o[i] = o[i] * dg + (takes ? wg * lucWet[(size_t)i] : 0.0f);
        }
    }
    if (!on && (lucRing -= n) <= 0)
    {
        lucRing = 0;
        lucRunning = false;
        lucDry = 1.0f;
        lucWetGain = 0.0f;
    }
    lucierSerialInUse = lucier.oldestSerialInUse();
}

// ---------------------------------------------------------------------------
// audio input: one or two sources (left / right) placed and moved in the same space as the pipes
// ---------------------------------------------------------------------------
void OrganEngine::followInput(int start, int len, const EngineParams &p) noexcept
{
    // loudness of the input (after its level) as a modulation source - also while the input itself
    // is not heard
    double e = 0.0;
    const float *l = inPtr[0];
    const float *r = inPtr[1] != nullptr ? inPtr[1] : l;
    if (l != nullptr && start + len <= inLen)
        for (int i = start; i < start + len; ++i)
            e += 0.5 * ((double)l[i] * l[i] + (double)r[i] * r[i]);
    const float pw = (float)(e / juce::jmax(1, len)) * p.input.level * p.input.level;
    const float k = 1.0f - std::exp(-(float)len / ((pw > inEnv ? 0.01f : 0.2f) * (float)sr));
    inEnv += k * (pw - inEnv);
    const float db = 10.0f * std::log10(inEnv + 1.0e-12f);
    inFollowVal = juce::jlimit(0.0f, 1.0f, (db + 54.0f) / 54.0f);
}

void OrganEngine::renderInput(juce::AudioBuffer<float> &out, int n, const EngineParams &p) noexcept
{
    const auto &ip = p.input;
    if (n <= 0 || n > kSubBlock || inRing.empty())
        return;
    // raw peak for the meter
    {
        float pk = 0.0f;
        for (int c = 0; c < 2; ++c)
            if (inPtr[c] != nullptr && roomOffset + n <= inLen)
                pk = juce::jmax(
                    pk, juce::FloatVectorOperations::findMaximum(inPtr[c] + roomOffset, n),
                    -juce::FloatVectorOperations::findMinimum(inPtr[c] + roomOffset, n));
        inPeak = juce::jmax(pk, inPeak.load() * 0.93f);
    }
    if (!ip.on && !inRunning)
    {
        inShowCount = 0;
        return;
    }
    if (!inRunning)
    {
        std::fill(inRing.begin(), inRing.end(), 0.0f);
        inRunning = true;
        inGate = 0.0f;
        inLevelSm = 0.0f;
        for (auto &is : inSrc)
        {
            is.fresh = true;
            is.lp = 0.0f;
        }
    }

    // level and gate (keys open it), per sample
    std::array<float, kSubBlock> amp{};
    bool anyKey = false;
    for (int k = 0; k < kNumKeys && !anyKey; ++k)
        anyKey = keyHeld[(size_t)k] || keySustained[(size_t)k];
    const float gateT = ip.gate ? (anyKey ? 1.0f : 0.0f) : 1.0f;
    const float levelT = ip.on ? ip.level : 0.0f;
    const float ca = 1.0f - std::exp(-1.0f / (0.005f * (float)sr));
    const float cr = 1.0f - std::exp(-1.0f / (juce::jmax(0.01f, ip.release) * (float)sr));
    const float cl = 1.0f - std::exp(-1.0f / (0.02f * (float)sr));
    for (int i = 0; i < n; ++i)
    {
        inGate += (gateT > inGate ? ca : cr) * (gateT - inGate);
        inLevelSm += cl * (levelT - inLevelSm);
        amp[(size_t)i] = inGate * inLevelSm;
    }

    // places (end of this slice; ramped from the last ones)
    const double secs = (double)n / sr;
    inSpin = std::fmod(inSpin + (double)ip.spin * secs, 360.0);
    const int nSrc = ip.stereo ? 2 : 1;
    const int mid = kNumKeys / 2;
    for (int c = 0; c < 2; ++c)
    {
        auto &is = inSrc[(size_t)c];
        if (c >= nSrc)
        {
            releaseSlot(is.erSlot);
            is.fresh = true;
            continue;
        }
        Pos pos;
        if (ip.follow >= 0 && ip.follow < kNumStops)
        {
            // riding on the pipes of a stop: left on the low side, right on the high side (width =
            // how far apart)
            const int w =
                nSrc == 2 ? juce::jlimit(0, mid, juce::roundToInt(ip.width / 180.0f * (float)mid))
                          : 0;
            const int key = juce::jlimit(0, kNumKeys - 1, c == 0 ? mid - w : mid + w);
            pos = currentPosition(ip.follow, key, p);
            pos.az = wrapAz(pos.az + ip.az + (float)inSpin);
        }
        else
        {
            const float off = nSrc == 2 ? (c == 0 ? 0.5f : -0.5f) * ip.width : 0.0f;
            pos = {wrapAz(ip.az + off + (float)inSpin), juce::jlimit(-90.0f, 90.0f, ip.el),
                   juce::jlimit(kMinDist, kMaxDist, ip.dist)};
        }
        is.pos = pos;
        gainsFor(pos, is.g1.data());
        const float dg = distanceGain(pos.dist, p.distAmount);
        for (auto &g : is.g1)
            g *= dg;
        is.send1 = distanceGain(pos.dist, p.distAmount * 0.35f) * ip.send;
        is.sendAz1 = p.roomFollow ? pos.az : p.roomBaseAz;
        // distance takes time (sound travels): moving sources are Doppler-shifted, like the pipes
        is.d1 = p.doppler > 0.0f
                    ? juce::jlimit(0.0f, (float)(kInRing - 4 * kSubBlock),
                                   p.doppler * (pos.dist - kMinDist) / kSpeedOfSound * (float)sr)
                    : 0.0f;
        const float fc =
            juce::jlimit(1200.0f, 20000.0f, 20000.0f / (1.0f + p.air * pos.dist / 5.0f));
        is.airA = fc >= 19999.0f
                      ? 1.0f
                      : 1.0f - std::exp(-juce::MathConstants<float>::twoPi * fc / (float)sr);
        if (is.fresh)
        {
            is.g0 = is.g1;
            is.send0 = is.send1;
            is.sendAz0 = is.sendAz1;
            is.d0 = is.d1;
            is.fresh = false;
        }
        if (roomOnNow && ip.on)
        {
            if (is.erSlot < 0)
            {
                is.erSlot = allocSlot(-1, -1);
                if (is.erSlot >= 0)
                    erSlots[(size_t)is.erSlot].input = c;
            }
        }
        else
            releaseSlot(is.erSlot);
        inShowAz[(size_t)c] = pos.az;
        inShowEl[(size_t)c] = pos.el;
        inShowDist[(size_t)c] = pos.dist;
    }
    inShowCount = ip.on ? nSrc : 0;

    // into the delay lines
    const float *l = inPtr[0];
    const float *r = inPtr[1] != nullptr ? inPtr[1] : l;
    const bool have = l != nullptr && roomOffset + n <= inLen;
    for (int i = 0; i < n; ++i)
    {
        const uint32_t w = (inClock + (uint32_t)i) & (kInRing - 1);
        const float a = have ? l[roomOffset + i] : 0.0f, b = have ? r[roomOffset + i] : 0.0f;
        const float g = amp[(size_t)i];
        if (nSrc == 2)
        {
            inRing[w] = a * g;
            inRing[(size_t)kInRing + w] = b * g;
        }
        else
        {
            inRing[w] = 0.5f * (a + b) * g;
            inRing[(size_t)kInRing + w] = 0.0f;
        }
    }

    // out of them: delayed, air-filtered, placed, into the room
    std::array<float, kSubBlock> buf{}, bufRamp{};
    const float inv = 1.0f / (float)n;
    for (int c = 0; c < nSrc; ++c)
    {
        auto &is = inSrc[(size_t)c];
        const float *ring = inRing.data() + (size_t)c * (size_t)kInRing;
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float d = is.d0 + (is.d1 - is.d0) * (float)(i + 1) * inv;
            const int di = (int)d;
            const float fd = d - (float)di;
            const uint32_t at = inClock + (uint32_t)i - (uint32_t)di;
            const float x0 = ring[(at - 1u) & (kInRing - 1)], x1 = ring[at & (kInRing - 1)];
            is.lp += is.airA * (x1 + fd * (x0 - x1) - is.lp);
            buf[(size_t)i] = is.lp;
            bufRamp[(size_t)i] = is.lp * (float)i;
            peak = juce::jmax(peak, std::abs(is.lp));
        }
        if (peak > 1.0e-9f)
        {
            for (int ch = 0; ch < usedChannels; ++ch)
            {
                const float a = is.g0[(size_t)ch], b = is.g1[(size_t)ch];
                const float dg = (b - a) * inv;
                float *o = out.getWritePointer(ch);
                if (std::abs(a) > 1.0e-9f)
                    juce::FloatVectorOperations::addWithMultiply(o, buf.data(), a, n);
                if (std::abs(dg) > 1.0e-12f)
                    juce::FloatVectorOperations::addWithMultiply(o, bufRamp.data(), dg, n);
            }
            const double a0 = juce::degreesToRadians((double)is.sendAz0),
                         a1 = juce::degreesToRadians((double)is.sendAz1);
            for (int k = 0; k < roomSends; ++k)
            {
                const int m = (k + 1) / 2;
                const float w0 =
                    k == 0 ? 1.0f : (float)(k % 2 == 1 ? std::cos(m * a0) : std::sin(m * a0));
                const float w1 =
                    k == 0 ? 1.0f : (float)(k % 2 == 1 ? std::cos(m * a1) : std::sin(m * a1));
                const float g0 = is.send0 * w0, g1 = is.send1 * w1;
                float *dst = roomSend[(size_t)k].data() + roomOffset;
                if (std::abs(g0) > 1.0e-12f)
                    juce::FloatVectorOperations::addWithMultiply(dst, buf.data(), g0, n);
                if (std::abs(g1 - g0) > 1.0e-12f)
                    juce::FloatVectorOperations::addWithMultiply(dst, bufRamp.data(),
                                                                 (g1 - g0) * inv, n);
                if (k == 0 && p.inputRoom > 0 && roomOnNow)
                {
                    float *d2 =
                        rt[(size_t)juce::jlimit(0, kNumRooms - 1, p.inputRoom)].send.data() +
                        roomOffset;
                    if (std::abs(g0) > 1.0e-12f)
                        juce::FloatVectorOperations::addWithMultiply(d2, buf.data(), g0, n);
                    if (std::abs(g1 - g0) > 1.0e-12f)
                        juce::FloatVectorOperations::addWithMultiply(d2, bufRamp.data(),
                                                                     (g1 - g0) * inv, n);
                }
            }
            if (is.erSlot >= 0)
            {
                float *er = erRing.data() + (size_t)is.erSlot * (size_t)kErRing;
                for (int i = 0; i < n; ++i)
                    er[(erClock + (uint32_t)i) & (kErRing - 1)] += buf[(size_t)i];
            }
        }
        is.g0 = is.g1;
        is.send0 = is.send1;
        is.sendAz0 = is.sendAz1;
        is.d0 = is.d1;
    }
    inClock += (uint32_t)n;

    // switched off: runs on until the delay lines and the fade are empty
    if (ip.on || inLevelSm >= 1.0e-5f)
        inDrain = -1;
    else if (inDrain < 0)
        inDrain = (int)juce::jmax(inSrc[0].d1, inSrc[1].d1) + 2 * kSubBlock;
    else if ((inDrain -= n) <= 0)
    {
        inDrain = -1;
        inRunning = false;
        for (auto &is : inSrc)
        {
            releaseSlot(is.erSlot);
            is.fresh = true;
        }
        inShowCount = 0;
    }
}

void OrganEngine::processChunk(juce::AudioBuffer<float> &out, const juce::MidiBuffer &midi,
                               const EngineParams &pIn)
{
    EngineParams p = pIn; // local copy with derived values
    double profT = profOn ? juce::Time::getMillisecondCounterHiRes() : 0.0;
    auto profMark = [&](int stage)
    {
        if (!profOn)
            return;
        const double now = juce::Time::getMillisecondCounterHiRes();
        prof[(size_t)stage] += now - profT;
        profT = now;
    };
    p.deg.prepareBlock(sr);
    out.clear();
    const int numSamples = out.getNumSamples();
    if (numSamples <= 0)
        return;
    if ((int)scratch.size() < numSamples) // safety (should not happen: prepare() sizes them)
    {
        scratch.assign((size_t)numSamples, 0.0f);
        scratchRamp.assign((size_t)numSamples, 0.0f);
        hidBuf.assign(2 * (size_t)numSamples, 0.0f);
        nearL.assign((size_t)numSamples, 0.0f);
        nearR.assign((size_t)numSamples, 0.0f);
    }
    roomSends = 2 * juce::jlimit(0, kMaxOrder, p.roomHarmonics) + 1;
    for (int k = 0; k < roomSends; ++k)
        std::fill(roomSend[(size_t)k].begin() + roomOffset,
                  roomSend[(size_t)k].begin() + roomOffset + numSamples, 0.0f);
    for (int ri = 1; ri < kNumRooms; ++ri)
        std::fill(rt[(size_t)ri].send.begin() + roomOffset,
                  rt[(size_t)ri].send.begin() + roomOffset + numSamples, 0.0f);
    {
        // clicks and stop pulls come from the console (or the common direction when not following)
        const double a =
            juce::degreesToRadians((double)(p.roomFollow ? p.consoleAz : p.roomBaseAz));
        noiseRoomW[0] = 1.0f;
        for (int m = 1; 2 * m < roomSends; ++m)
        {
            noiseRoomW[(size_t)(2 * m - 1)] = (float)std::cos(m * a);
            noiseRoomW[(size_t)(2 * m)] = (float)std::sin(m * a);
        }
    }
    const double blockSeconds = numSamples / sr;

    order = juce::jlimit(0, kMaxOrder, p.outputMode);
    usedChannels = juce::jmin(out.getNumChannels(), order == 0 ? 2 : (order + 1) * (order + 1));
    distAmountNow = p.distAmount;
    consoleAzNow = p.consoleAz;
    consoleElNow = p.consoleEl;
    if (roomOnNow && !p.room.on)
    {
        // room switched off: reflections stop
        for (auto &v : voices)
            v.erSlot = -1;
        for (auto &nv : noise)
            nv.erSlot = -1;
        for (auto &s : erSlots)
            s.active = false;
    }
    roomOnNow = p.room.on && !erSlots.empty();
    if (roomOnNow)
        for (size_t si = 0; si < erSlots.size(); ++si)
            if (erSlots[si].active)
            {
                // this chunk's part of the ring is rewritten by the owner (or stays silent while
                // ringing out)
                float *ring = erRing.data() + si * (size_t)kErRing;
                for (int i = 0; i < numSamples; ++i)
                    ring[(erClock + (uint32_t)i) & (kErRing - 1)] = 0.0f;
            }
    keySource = p.tuning.source;
    {
        // Per-note routing keeps two MIDI keys independent when they share a pipe.
        mpeNow = p.mpe;
        mpeBendCents = 100.0f * p.mpeBendRange;
        const NoteTable *table = p.notes;
        const bool fillChromaticGaps = p.correctSamplePitch;
        bool mappingChanged = false;
        if (table == nullptr && (mpeNow || fillChromaticGaps))
        {
            const bool extended = fillChromaticGaps && p.extendedRange;
            const int first = extended ? kExtendedLowestMidiNote : kPhysKeys.front();
            const int last = extended ? kExtendedHighestMidiNote : kPhysKeys.back();
            for (int n = kExtendedLowestMidiNote; n <= kExtendedHighestMidiNote; ++n)
            {
                const auto index = static_cast<std::size_t>(n);
                const bool mapped =
                    n >= first && n <= last && (fillChromaticGaps || keyIndexForMidi(n) >= 0);
                mappingChanged |= (keyboardTable.mapped[index] != 0) != mapped;
                keyboardTable.mapped[index] = mapped ? 1 : 0;
                keyboardTable.hz[index] =
                    mapped ? static_cast<float>(keyboardTuningHz(p.tuning, n)) : 0.0f;
            }
            table = &keyboardTable;
        }
        const bool fm = table != nullptr;
        const bool tableChanged = table != notesNow;
        if (fm != freeMode)
        {
            releaseAllKeys(p);
            freeMode = fm;
        }
        notesNow = table;
        if (notesNow != nullptr && (tableChanged || mappingChanged))
            for (int id = 0; id < NoteTable::kSize; ++id)
                if (freeNotes[static_cast<std::size_t>(id)].held &&
                    !notesNow->mapped[static_cast<std::size_t>(tableIndex(id))])
                    keyUp(freeNotes[static_cast<std::size_t>(id)].key, p, id);

        // A held voice keeps its sample and phase when the tuning source changes.
        // Refresh its offset for the new table even when MTS follow is disabled.
        notesContinuous = tableChanged || table == &keyboardTable || p.notesContinuous;
    }
    roomFollowNow = p.roomFollow;
    roomBaseAzNow = p.roomBaseAz;
    chunkSeconds = (float)blockSeconds;

    // ---- stops pulled / pushed
    if (firstBlock)
    {
        lastStopOn = p.stopOn;
        firstBlock = false;
    }
    for (int s = 0; s < kNumStops; ++s)
    {
        if (p.stopOn[(size_t)s] == lastStopOn[(size_t)s])
            continue;
        std::array<float, kMaxAmbiChannels> console{};
        gainsFor({p.consoleAz, p.consoleEl}, console.data());
        if (library != nullptr)
            playNoise(library->stopPulls, p.stopPull * (p.stopOn[(size_t)s] ? 1.4f : 1.1f),
                      console.data(), lastPull, 0.5f);
        for (int k = 0; k < kNumKeys; ++k)
        {
            if (p.stopOn[(size_t)s] && keyHeld[(size_t)k] && !freeMode && p.noteSpread == 0)
                startVoice(s, k);
            if (!p.stopOn[(size_t)s])
                releaseStopKey(s, k);
        }
        if (p.stopOn[(size_t)s] && freeMode && p.noteSpread == 0)
            for (int id = 0; id < NoteTable::kSize; ++id)
                if (freeNotes[(size_t)id].held)
                    startVoice(s, freeNotes[(size_t)id].key, -1, id);
    }
    lastStopOn = p.stopOn;

    // ---- freeze switched off: frozen pipes fade out
    if (!p.freeze)
    {
        const float step = (float)(1.0 / (juce::jmax(0.05f, p.freezeRelease) * sr));
        for (auto &v : voices)
            if (v.active && v.frozen && !v.killing)
            {
                v.killing = true;
                v.killStep = step;
            }
    }

    // ---- previews requested by the UI
    {
        const auto scope = previewFifo.read(previewFifo.getNumReady());
        auto doOne = [this](int idx)
        {
            auto r = previewQueue[(size_t)idx];
            startVoice(r.first, r.second, (int)(1.6 * sr));
        };
        for (int i = 0; i < scope.blockSize1; ++i)
            doOne(scope.startIndex1 + i);
        for (int i = 0; i < scope.blockSize2; ++i)
            doOne(scope.startIndex2 + i);
    }

    // ---- motion phases: speed * time, or locked to the host's musical position
    for (int s = 0; s < kNumStops; ++s)
    {
        double ph;
        if (p.sync && p.playing)
        {
            // speeds count at 120 BPM (a quarter note = 0.5 s): cycles = speed * quarters * 0.5
            const double ppqEnd = p.ppq + blockSeconds * p.bpm / 60.0;
            ph = (double)p.motionRate[(size_t)s] * ppqEnd * 0.5;
        }
        else
        {
            // (the player holds everything still: "freeze" in the 3D view)
            const double scale = actFreeze.load() ? 0.0 : (p.sync ? p.bpm / 120.0 : 1.0);
            ph = motionPhase[(size_t)s].load() +
                 (double)p.motionRate[(size_t)s] * scale * blockSeconds;
        }
        if (std::abs(ph) > 1.0e7)
            ph = std::fmod(ph, 1000.0);
        // hard jumps need the phase at every sample of this block: start and end (a relocation = no
        // movement)
        phasePrev[(size_t)s] = motionPhase[(size_t)s].load();
        if (std::abs(ph - phasePrev[(size_t)s]) > 1000.0)
            phasePrev[(size_t)s] = ph;
        phaseNow[(size_t)s] = ph;
        motionPhase[(size_t)s] = ph;
    }

    // ---- effects: due events (delayed sounds, pipes waiting for the tracker path), moving sounds
    {
        int w = 0;
        for (int i = 0; i < numScheduled; ++i)
        {
            auto e = scheduled[(size_t)i];
            if (e.delay >= numSamples)
            {
                e.delay -= numSamples;
                scheduled[(size_t)w++] = e;
                continue;
            }
            if (!e.voice)
            {
                if (e.sample != nullptr && library != nullptr)
                    spawnSound(*e.sample, e.gain, e.tone, e.rate, e.pos, e.azVel, e.send);
            }
            else if (library != nullptr && p.stopOn[(size_t)e.stop])
            {
                startVoice(e.stop, e.key, -1, e.noteId);
                // key already let go: the pallet still opened, but closes right away
                const bool stillDown =
                    e.noteId >= 0 ? freeNotes[(size_t)e.noteId].held
                                  : (keyHeld[(size_t)e.key] || keySustained[(size_t)e.key]);
                if (!stillDown)
                    for (auto &v : voices)
                        if (v.active && v.held && v.stop == e.stop && v.key == e.key &&
                            v.previewRemain < 0 && v.noteId == e.noteId)
                            releaseVoice(v);
            }
        }
        numScheduled = w;
    }
    for (auto &nv : noise)
        if (nv.active && nv.azVel != 0.0f)
        {
            nv.where.az = wrapAz(nv.where.az + nv.azVel * (float)blockSeconds);
            placedGains(nv);
        }
    partRot = std::fmod(partRot + (double)p.fx.partTurn * 360.0 * blockSeconds, 360.0);

    updateGrid(p.space.grid);
    pipeSndNow = p.pipeSnd;
    glitchNow = p.glitch;
    // ---- the living organ: bellows, the walking listener, wear, pure tuning, sympathetic pipes;
    // the Tonnetz walker
    {
        ProfScope profBodies(profOn, prof[6]);
        stepBodies(p, blockSeconds); // the room motion modes: the pipes as bodies in the Saal (and their impacts)
    }
    stepGrowth(p, blockSeconds);      // Wachsen & Brechen: the pipes' lengths
    startSelfVoices(p, blockSeconds); // Selbstspiel: knocks become notes
    {
        // Fluestern: the two ear signals of this block
        const bool had = nearUsed;
        nearUsed = false;
        if ((had || p.live.whisper > 0.0001f) && (int)nearL.size() >= numSamples)
        {
            std::fill_n(nearL.data(), numSamples, 0.0f);
            std::fill_n(nearR.data(), numSamples, 0.0f);
        }
    }
    updateLive(p, blockSeconds);
    stepWalker(p, numSamples);
    // ---- spatial data for this block (end-of-block targets, ramped from the previous ones)
    const double profSpatial = profOn ? juce::Time::getMillisecondCounterHiRes() : 0.0;
    for (auto &v : voices)
    {
        if (!v.active)
            continue;
        if (v.noteId >= 0 && notesContinuous && v.held && notesNow != nullptr &&
            notesNow->hz[(size_t)tableIndex(v.noteId)] > 1.0f)
            v.noteCents = centsFor(v.noteId, v.key); // MTS-ESP: held notes follow a changing tuning
        if (!v.fresh)
        {
            v.g0 = v.g1;
            v.gh0 = v.gh1;
            v.send0 = v.send1;
            v.sendAz0 = v.sendAz1;
            v.modCents0 = v.modCents1;
        }
        updateVoiceSpatial(v, p, blockSeconds);
        if (v.fresh)
        {
            v.g0 = v.g1;
            v.gh0 = v.gh1;
            v.send0 = v.send1;
            v.sendAz0 = v.sendAz1;
            v.modCents0 = v.modCents1;
            v.fresh = false;
        }
    }
    if (profOn)
        prof[7] += juce::Time::getMillisecondCounterHiRes() - profSpatial;
    updateBeats(p, blockSeconds);
    auto initFresh = [&]()
    {
        for (auto &v : voices)
            if (v.active && v.fresh)
            {
                updateVoiceSpatial(v, p, blockSeconds);
                v.g0 = v.g1;
                v.gh0 = v.gh1;
                v.send0 = v.send1;
                v.sendAz0 = v.sendAz1;
                v.modCents0 = v.modCents1;
                v.fresh = false;
                v.dop = v.dopTarget;
            }
    };

    profMark(0);
    // ---- render, split at MIDI events
    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit(0, numSamples, meta.samplePosition);
        renderRange(out, pos, at, numSamples, p);
        pos = at;
        handleMidi(meta.getMessage(), p);
        initFresh(); // voices started mid-block take the end-of-block values right away
    }
    initFresh(); // voices started by stop changes / previews before any MIDI
    renderRange(out, pos, numSamples, numSamples, p);
    profMark(1);
    renderLive(out.getArrayOfWritePointers(), numSamples, p);
    renderImpacts(out.getArrayOfWritePointers(), numSamples,
                  p); // pipes flying in the Saal knocking on its walls
    renderLeaks(out, numSamples, p);
    renderInput(out, numSamples, p); // the audio input: placed like a pipe, then everything below
                                     // (wear, echoes, room, Lucier)

    // ---- degradation on the sum
    if (!p.deg.perPipe && p.deg.amount > 0.0001f)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const auto f = sumDeg.frame(p.deg, sr);
            for (int c = 0; c < usedChannels; ++c)
            {
                float *o = out.getWritePointer(c);
                o[i] = DegradeState::process(o[i], sumHeld[(size_t)c], sumEnv[(size_t)c], f, p.deg);
            }
        }
    }

    renderEchoes(out, numSamples, p);
    profMark(2);
    renderRoom(out, numSamples, p);

    // ---- "Raumverfall" on the Ambisonics channels (the plugin does it after the impulse response
    // instead)
    if (spaceDegInProcess && order > 0)
        applySpaceDegradation(out.getArrayOfWritePointers(), juce::jmin(order, kMaxOrder),
                              numSamples, p);
    spaceErosionNow = eroder.erosionNow(p.space);
    cloudGrainsNow = cloudGrainsActive;

    // ---- master gain (smoothed) + meters
    const float step = (p.masterGain - masterSmoothed) / (float)numSamples;
    for (int c = 0; c < usedChannels; ++c)
    {
        float g = masterSmoothed;
        float *o = out.getWritePointer(c);
        for (int i = 0; i < numSamples; ++i)
        {
            g += step;
            o[i] *= g;
        }
    }
    {
        for (int k = 0; k < roomSends; ++k)
        {
            float gk = masterSmoothed;
            for (int i = 0; i < numSamples; ++i)
            {
                gk += step;
                roomSend[(size_t)k][(size_t)(roomOffset + i)] *= gk;
            }
        }
    }
    if (nearUsed)
    {
        float g = masterSmoothed;
        for (int i = 0; i < numSamples; ++i)
        {
            g += step;
            nearL[(size_t)i] *= g;
            nearR[(size_t)i] *= g;
        }
    }
    masterSmoothed = p.masterGain;

    // ---- bookkeeping for the UI
    int count = 0, frozen = 0;
    std::array<uint64_t, kNumStops> sounding{};
    for (auto &v : voices)
        if (v.active)
        {
            ++count;
            frozen += v.frozen ? 1 : 0;
            sounding[(size_t)v.stop] |= (uint64_t)1 << v.key;
        }
    for (int s = 0; s < kNumStops; ++s)
        soundingMask[(size_t)s] = sounding[(size_t)s];
    activeVoices = count;
    frozenVoices = frozen;
    uint64_t mask = 0;
    for (int k = 0; k < kNumKeys; ++k)
        if (keyHeld[(size_t)k] || keySustained[(size_t)k])
            mask |= (uint64_t)1 << k;
    heldMask = mask;
    timeSeconds = timeSeconds.load() + blockSeconds;
}
} // namespace rysum
