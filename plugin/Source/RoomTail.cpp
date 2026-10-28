#include "RoomTail.h"

namespace rysum
{
int TailIR::maxLength (double sr) noexcept
{
    const int b = kBlock[kStages - 1];
    const int n = (int) std::ceil (kMaxSeconds * sr / (double) b) * b;
    return juce::jmax (kStart[kStages - 1] + 2 * b, n);
}

std::shared_ptr<const TailIR> TailIR::build (const RoomParams& r, float air, int outputOrder, double sr, uint32_t serial)
{
    auto ir = std::make_shared<TailIR>();
    ir->serial = serial;
    ir->sampleRate = sr;
    ir->order = outputOrder <= 0 ? 0 : juce::jmin (outputOrder, 2);       // a diffuse tail: 2nd order is plenty
    ir->channels = ir->order == 0 ? 2 : (ir->order + 1) * (ir->order + 1);

    // ---- decay per band from ray tracing
    const auto res = traceRoom (r, air, 4000, 1234u);        // same rays every time: no random jumps while modulating
    ir->rtLow = juce::jlimit (0.05f, 30.0f, res.rtLow);
    ir->rtHigh = juce::jlimit (0.05f, 30.0f, juce::jmin (res.rtHigh, res.rtLow));
    ir->rays = res.rays;
    ir->volume = roomVolume (r);

    // ---- synthesis: decorrelated noise per channel, two bands with their own decay
    const double mfpSec = meanFreePath (r) / kSpeedOfSoundRoom;
    const int pre = juce::jmax (kStart[0], (int) std::lround (0.5 * mfpSec * sr));          // tail begins after the first reflections
    const int fadeIn = juce::jmax (64, (int) std::lround (mfpSec * sr));
    const int maxLen = maxLength (sr);
    const int len = juce::jlimit (pre + fadeIn + 512, maxLen, (int) std::ceil (1.2 * juce::jmax (ir->rtLow, ir->rtHigh) * sr) + pre);
    ir->length = len;
    const int fadeOut = len / 10;
    const double kL = 6.9078 / (ir->rtLow * sr), kH = 6.9078 / (ir->rtHigh * sr);
    std::vector<std::vector<float>> h ((size_t) ir->channels, std::vector<float> ((size_t) len, 0.0f));
    // The noise is the same for every rebuild (fixed seed per channel, indexed from t = 0): when the room is
    // modulated, successive responses differ only in their envelopes, so the reverb of a held tone changes smoothly
    // instead of jumping to a new random level on every rebuild.
    for (int c = 0; c < ir->channels; ++c)
    {
        XorShift rng (0x5eedu + (uint32_t) c * 7919u);
        float wgt = 1.0f;
        if (ir->order == 0) wgt = std::sqrt (0.5f);
        else { const int l = (int) std::floor (std::sqrt ((double) c)); wgt = 1.0f / std::sqrt ((float) (2 * l + 1)); }
        juce::IIRFilter lpF;
        lpF.setCoefficients (juce::IIRCoefficients::makeLowPass (sr, 1500.0));
        auto& x = h[(size_t) c];
        for (int t = 0; t < len; ++t)
        {
            // Gaussian-like noise (sum of four uniforms)
            const float nse = 0.866f * (rng.bi() + rng.bi() + rng.bi() + rng.bi());
            const float lo = lpF.processSingleSampleRaw (nse), hi = nse - lo;
            if (t < pre) continue;
            const double tt = (double) (t - pre);
            float a = (float) (lo * std::exp (-kL * tt) + hi * std::exp (-kH * tt));
            if (t - pre < fadeIn) a *= 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (t - pre) / (float) fadeIn);
            if (t >= len - fadeOut) a *= 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (t - (len - fadeOut)) / (float) fadeOut);
            x[(size_t) t] = a * wgt;
        }
    }
    // unit energy in W (stereo: in L + R together): the engine's gain sets the level
    double e = 0.0;
    for (int c = 0; c < (ir->order == 0 ? 2 : 1); ++c) for (auto v : h[(size_t) c]) e += (double) v * v;
    const float norm = e > 0 ? (float) (1.0 / std::sqrt (e)) : 0.0f;
    for (auto& ch : h) for (auto& v : ch) v *= norm;

    // ---- partitions per stage: segment p of a stage is h[(p+1)B, (p+2)B) (the stage runs one block late)
    for (int s = 0; s < kStages; ++s)
    {
        const int B = kBlock[s];
        const int nb = bins (s);
        int p0 = kStart[s] / B - 1, p1 = stageEnd (s, sr) / B - 1;
        p1 = juce::jmin (p1, (len + B - 1) / B - 1);         // nothing after the end
        if (p1 < p0) p1 = p0;
        ir->p0[(size_t) s] = p0; ir->p1[(size_t) s] = p1;
        const int np = p1 - p0;
        ir->re[(size_t) s].assign ((size_t) ir->channels * (size_t) juce::jmax (1, np) * (size_t) nb, 0.0f);
        ir->im[(size_t) s].assign (ir->re[(size_t) s].size(), 0.0f);
        int order = 0; while ((1 << order) < 2 * B) ++order;
        juce::dsp::FFT fft (order);
        std::vector<float> buf ((size_t) (4 * B));
        for (int c = 0; c < ir->channels; ++c)
            for (int p = p0; p < p1; ++p)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                const int a = (p + 1) * B;
                for (int i = 0; i < B && a + i < len; ++i) buf[(size_t) i] = h[(size_t) c][(size_t) (a + i)];
                fft.performRealOnlyForwardTransform (buf.data(), true);
                const size_t base = ((size_t) c * (size_t) np + (size_t) (p - p0)) * (size_t) nb;
                for (int k = 0; k < nb; ++k) { ir->re[(size_t) s][base + (size_t) k] = buf[(size_t) (2 * k)]; ir->im[(size_t) s][base + (size_t) k] = buf[(size_t) (2 * k + 1)]; }
            }
    }
    return ir;
}

// ---------------------------------------------------------------------------
void TailConvolver::prepare (double sampleRate)
{
    sr = sampleRate;
    for (int s = 0; s < TailIR::kStages; ++s)
    {
        auto& st = stages[(size_t) s];
        st.B = TailIR::kBlock[s];
        st.P = juce::jmax (1, TailIR::stageEnd (s, sr) / st.B - 1);
        int order = 0; while ((1 << order) < 2 * st.B) ++order;
        st.fft = std::make_unique<juce::dsp::FFT> (order);
        const int nb = TailIR::bins (s);
        st.inBuf.assign ((size_t) (2 * st.B), 0.0f);
        st.work.assign ((size_t) (4 * st.B), 0.0f);
        st.fdlRe.assign ((size_t) st.P * (size_t) nb, 0.0f);
        st.fdlIm.assign (st.fdlRe.size(), 0.0f);
        st.accRe.assign ((size_t) nb, 0.0f);
        st.accIm.assign ((size_t) nb, 0.0f);
        st.outA.assign ((size_t) kMaxCh * (size_t) st.B, 0.0f);
        st.outB.assign (st.outA.size(), 0.0f);
    }
    reset();
}

void TailConvolver::reset() noexcept
{
    for (auto& st : stages)
    {
        std::fill (st.inBuf.begin(), st.inBuf.end(), 0.0f);
        std::fill (st.fdlRe.begin(), st.fdlRe.end(), 0.0f);
        std::fill (st.fdlIm.begin(), st.fdlIm.end(), 0.0f);
        std::fill (st.outA.begin(), st.outA.end(), 0.0f);
        std::fill (st.outB.begin(), st.outB.end(), 0.0f);
        st.pos = 0; st.fdlPos = 0; st.readyB = false;
    }
}

void TailConvolver::setIR (std::shared_ptr<const TailIR> ir) noexcept
{
    if (ir == nullptr) return;
    if (cur == nullptr) { cur = std::move (ir); return; }
    if (next == nullptr && fade < 0)
    {
        next = std::move (ir);
        for (auto& st : stages) st.readyB = false;
    }
    else queued = std::move (ir);          // a newer one while fading: it follows right after
}

void TailConvolver::adoptLatest() noexcept
{
    // not sounding: take the newest response directly, no crossfade needed (the processor keeps the old one alive)
    if (queued != nullptr) { cur = std::move (queued); next.reset(); }
    else if (next != nullptr) cur = std::move (next);
    fade = -1;
    for (auto& st : stages) st.readyB = false;
}

uint32_t TailConvolver::oldestSerialInUse() const noexcept
{
    uint32_t s = 0xffffffffu;
    for (auto* p : { cur.get(), next.get(), queued.get() }) if (p != nullptr) s = juce::jmin (s, p->serial);
    return s;
}

void TailConvolver::convolve (Stage& st, int s, const TailIR& ir, std::vector<float>& out) noexcept
{
    const int nb = TailIR::bins (s);
    const int p0 = ir.p0[(size_t) s], p1 = ir.p1[(size_t) s], np = p1 - p0;
    const int chans = juce::jmin (ir.channels, kMaxCh);
    for (int c = 0; c < chans; ++c)
    {
        float* ar = st.accRe.data();
        float* ai = st.accIm.data();
        std::fill (st.accRe.begin(), st.accRe.end(), 0.0f);
        std::fill (st.accIm.begin(), st.accIm.end(), 0.0f);
        for (int p = p0; p < p1; ++p)
        {
            const int slot = (st.fdlPos + p) % st.P;
            const float* xr = st.fdlRe.data() + (size_t) slot * (size_t) nb;
            const float* xi = st.fdlIm.data() + (size_t) slot * (size_t) nb;
            const size_t base = ((size_t) c * (size_t) np + (size_t) (p - p0)) * (size_t) nb;
            const float* hr = ir.re[(size_t) s].data() + base;
            const float* hi = ir.im[(size_t) s].data() + base;
            for (int k = 0; k < nb; ++k) { ar[k] += xr[k] * hr[k] - xi[k] * hi[k]; ai[k] += xr[k] * hi[k] + xi[k] * hr[k]; }
        }
        for (int k = 0; k < nb; ++k) { st.work[(size_t) (2 * k)] = ar[k]; st.work[(size_t) (2 * k + 1)] = ai[k]; }
        std::fill (st.work.begin() + 2 * nb, st.work.end(), 0.0f);
        st.fft->performRealOnlyInverseTransform (st.work.data());
        std::copy (st.work.begin() + st.B, st.work.begin() + 2 * st.B, out.begin() + (std::ptrdiff_t) ((size_t) c * (size_t) st.B));
    }
    for (int c = chans; c < kMaxCh; ++c) std::fill_n (out.begin() + (std::ptrdiff_t) ((size_t) c * (size_t) st.B), st.B, 0.0f);
}

void TailConvolver::runStage (int s) noexcept
{
    auto& st = stages[(size_t) s];
    const int nb = TailIR::bins (s);
    std::copy (st.inBuf.begin(), st.inBuf.end(), st.work.begin());
    std::fill (st.work.begin() + 2 * st.B, st.work.end(), 0.0f);
    st.fft->performRealOnlyForwardTransform (st.work.data(), true);
    st.fdlPos = (st.fdlPos + st.P - 1) % st.P;
    float* xr = st.fdlRe.data() + (size_t) st.fdlPos * (size_t) nb;
    float* xi = st.fdlIm.data() + (size_t) st.fdlPos * (size_t) nb;
    for (int k = 0; k < nb; ++k) { xr[k] = st.work[(size_t) (2 * k)]; xi[k] = st.work[(size_t) (2 * k + 1)]; }
    std::copy (st.inBuf.begin() + st.B, st.inBuf.end(), st.inBuf.begin());

    if (cur != nullptr) convolve (st, s, *cur, st.outA);
    else std::fill (st.outA.begin(), st.outA.end(), 0.0f);
    if (next != nullptr) { convolve (st, s, *next, st.outB); st.readyB = true; }
}

void TailConvolver::process (const float* in, float* const* out, int numOut, int n, float g0, float g1) noexcept
{
    const int chans = juce::jmin (numOut, kMaxCh);
    const float inv = n > 0 ? 1.0f / (float) n : 0.0f;
    for (int i = 0; i < n; ++i)
    {
        for (auto& st : stages) st.inBuf[(size_t) (st.B + st.pos)] = in[i];
        const float g = g0 + (g1 - g0) * (float) (i + 1) * inv;
        const float f = fade >= 0 ? (float) fade / (float) kFade : 0.0f;
        for (int c = 0; c < chans; ++c)
        {
            float a = 0.0f, b = 0.0f;
            for (auto& st : stages)
            {
                a += st.outA[(size_t) c * (size_t) st.B + (size_t) st.pos];
                if (fade >= 0) b += st.outB[(size_t) c * (size_t) st.B + (size_t) st.pos];
            }
            out[c][i] += (fade >= 0 ? a + (b - a) * f : a) * g;
        }
        for (int s = 0; s < TailIR::kStages; ++s)
        {
            auto& st = stages[(size_t) s];
            if (++st.pos == st.B) { st.pos = 0; runStage (s); }
        }
        // crossfade to the new response once every stage has it
        if (next != nullptr && fade < 0)
        {
            bool ready = true;
            for (auto& st : stages) ready = ready && st.readyB;
            if (ready) fade = 0;
        }
        if (fade >= 0 && ++fade >= kFade)
        {
            for (auto& st : stages) { std::swap (st.outA, st.outB); st.readyB = false; }
            cur = next;                     // the old one is released here: the processor keeps a copy (freed on its thread)
            next = queued;
            queued = nullptr;
            fade = -1;
        }
    }
}
} // namespace rysum
