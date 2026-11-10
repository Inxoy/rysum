#include "RoomTail.h"

namespace rysum
{
int TailIR::maxLength(double sr) noexcept
{
    const int b = kBlock[kStages - 1];
    const int n = (int)std::ceil(kMaxSeconds * sr / (double)b) * b;
    return juce::jmax(kStart[kStages - 1] + 2 * b, n);
}

std::shared_ptr<const TailIR> TailIR::build(const RoomParams &r, float air, int outputOrder,
                                            double sr, uint32_t serial)
{
    auto ir = std::make_shared<TailIR>();
    ir->serial = serial;
    ir->sampleRate = sr;
    ir->order =
        outputOrder <= 0 ? 0 : juce::jmin(outputOrder, 2); // a diffuse tail: 2nd order is plenty
    ir->channels = ir->order == 0 ? 2 : (ir->order + 1) * (ir->order + 1);

    // ---- decay per band from ray tracing
    const auto res =
        traceRoom(r, air, 4000, 1234u); // same rays every time: no random jumps while modulating
    ir->rtLow = juce::jlimit(0.05f, 30.0f, res.rtLow);
    ir->rtHigh = juce::jlimit(0.05f, 30.0f, juce::jmin(res.rtHigh, res.rtLow));
    ir->rays = res.rays;
    ir->volume = roomVolume(r);

    // ---- synthesis: decorrelated noise per channel, two bands with their own decay
    const double mfpSec = meanFreePath(r) / kSpeedOfSoundRoom;
    const int pre = juce::jmax(
        kStart[0], (int)std::lround(0.5 * mfpSec * sr)); // tail begins after the first reflections
    const int fadeIn = juce::jmax(64, (int)std::lround(mfpSec * sr));
    const int maxLen = maxLength(sr);
    const int len =
        juce::jlimit(pre + fadeIn + 512, maxLen,
                     (int)std::ceil(1.2 * juce::jmax(ir->rtLow, ir->rtHigh) * sr) + pre);
    ir->length = len;
    const int fadeOut = len / 10;
    const double kL = 6.9078 / (ir->rtLow * sr), kH = 6.9078 / (ir->rtHigh * sr);
    std::vector<std::vector<float>> h((size_t)ir->channels, std::vector<float>((size_t)len, 0.0f));
    // The noise is the same for every rebuild (fixed seed per channel, indexed from t = 0): when
    // the room is modulated, successive responses differ only in their envelopes, so the reverb of
    // a held tone changes smoothly instead of jumping to a new random level on every rebuild.
    for (int c = 0; c < ir->channels; ++c)
    {
        XorShift rng(0x5eedu + (uint32_t)c * 7919u);
        float wgt = 1.0f;
        if (ir->order == 0)
            wgt = std::sqrt(0.5f);
        else
        {
            const int l = (int)std::floor(std::sqrt((double)c));
            wgt = 1.0f / std::sqrt((float)(2 * l + 1));
        }
        juce::IIRFilter lpF;
        lpF.setCoefficients(juce::IIRCoefficients::makeLowPass(sr, 1500.0));
        auto &x = h[(size_t)c];
        for (int t = 0; t < len; ++t)
        {
            // Gaussian-like noise (sum of four uniforms)
            const float nse = 0.866f * (rng.bi() + rng.bi() + rng.bi() + rng.bi());
            const float lo = lpF.processSingleSampleRaw(nse), hi = nse - lo;
            if (t < pre)
                continue;
            const double tt = (double)(t - pre);
            float a = (float)(lo * std::exp(-kL * tt) + hi * std::exp(-kH * tt));
            if (t - pre < fadeIn)
                a *= 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * (float)(t - pre) /
                                            (float)fadeIn);
            if (t >= len - fadeOut)
                a *= 0.5f + 0.5f * std::cos(juce::MathConstants<float>::pi *
                                            (float)(t - (len - fadeOut)) / (float)fadeOut);
            x[(size_t)t] = a * wgt;
        }
    }
    // unit energy in W (stereo: in L + R together): the engine's gain sets the level
    double e = 0.0;
    for (int c = 0; c < (ir->order == 0 ? 2 : 1); ++c)
        for (auto v : h[(size_t)c])
            e += (double)v * v;
    const float norm = e > 0 ? (float)(1.0 / std::sqrt(e)) : 0.0f;
    for (auto &ch : h)
        for (auto &v : ch)
            v *= norm;

    ir->partition(h);
    return ir;
}

std::shared_ptr<TailIR> TailIR::fromResponses(const std::vector<std::vector<float>> &h, double sr,
                                              uint32_t serial)
{
    auto ir = std::make_shared<TailIR>();
    ir->serial = serial;
    ir->sampleRate = sr;
    ir->channels = (int)h.size();
    ir->order = 0;
    ir->length = 0;
    for (auto &c : h)
        ir->length = juce::jmax(ir->length, juce::jmin((int)c.size(), maxLength(sr)));
    ir->partition(h);
    return ir;
}

void TailIR::partition(const std::vector<std::vector<float>> &h)
{
    const double sr = sampleRate;
    const int len = length;
    // ---- partitions per stage: partition q of a stage is h[(first + q + lag) B, ... + B), from
    // kStart up to the next stage (or the end of the response)
    for (int s = 0; s < kStages; ++s)
    {
        const int B = kBlock[s], N = 2 * B, L = kLag[s];
        const int q0 = kStart[s] / B - L;
        int q1 = stageEnd(s, sr) / B - L;
        q1 = juce::jmin(q1, (len + B - 1) / B - L); // nothing after the end
        if (q1 < q0)
            q1 = q0;
        first[(size_t)s] = q0;
        parts[(size_t)s] = q1 - q0;
        const int np = q1 - q0;
        spec[(size_t)s].assign((size_t)channels * (size_t)juce::jmax(1, np) * (size_t)N);
        RealFFT fft(N);
        AlignedFloats buf((size_t)N);
        for (int c = 0; c < channels; ++c)
            for (int q = q0; q < q1; ++q)
            {
                buf.clear();
                const int a = (q + L) * B;
                const auto &x = h[(size_t)c];
                for (int i = 0; i < B && a + i < len && a + i < (int)x.size(); ++i)
                    buf[(size_t)i] = x[(size_t)(a + i)];
                fft.forwardScaled(buf.data(),
                                  spec[(size_t)s].data() +
                                      ((size_t)c * (size_t)np + (size_t)(q - q0)) * (size_t)N);
            }
    }
}

// ---------------------------------------------------------------------------
void TailConvolver::prepare(double sampleRate)
{
    sr = sampleRate;
    for (int s = 0; s < TailIR::kStages; ++s)
    {
        const int B = TailIR::kBlock[s], L = TailIR::kLag[s];
        const int depth = juce::jmax(1, TailIR::stageEnd(s, sr) / B - L);
        stages[(size_t)s].prepare(B, L, 1, depth, 2 * kMaxCh, 1);
    }
    for (auto &a : appliedSerial)
        a.fill(kNone);
    for (auto &a : pendingSerial)
        a.fill(kNone);
    for (auto &a : isPending)
        a.fill(false);
    // (a response set before prepare: give it to the new stages)
    if (cur != nullptr)
        assign(curSet, cur.get());
    if (next != nullptr)
        assign(1 - curSet, next.get());
    reset();
}

void TailConvolver::reset() noexcept
{
    for (auto &st : stages)
        st.reset();
    trackApplied();
}

void TailConvolver::assign(int set, const TailIR *ir) noexcept
{
    for (int s = 0; s < TailIR::kStages; ++s)
    {
        auto &st = stages[(size_t)s];
        if (st.numOutputs() < 2 * kMaxCh)
            continue; // (not prepared yet)
        const int np = ir != nullptr ? ir->parts[(size_t)s] : 0;
        const int chans = ir != nullptr ? juce::jmin(ir->channels, kMaxCh) : 0;
        for (int c = 0; c < kMaxCh; ++c)
        {
            const int o = set * kMaxCh + c;
            if (c < chans && np > 0)
            {
                PartitionedConvolution::Term t;
                t.input = 0;
                t.first = ir->first[(size_t)s];
                t.parts = np;
                t.spectra = ir->spec[(size_t)s].data() +
                            (size_t)c * (size_t)np * (size_t)(2 * TailIR::kBlock[s]);
                st.setTerms(o, &t, 1);
            }
            else
                st.clearTerms(o);
        }
        isPending[(size_t)s][(size_t)set] = true;
        pendingSerial[(size_t)s][(size_t)set] = ir != nullptr ? ir->serial : kNone;
    }
}

void TailConvolver::trackApplied() noexcept
{
    for (int s = 0; s < TailIR::kStages; ++s)
        for (int set = 0; set < 2; ++set)
            if (isPending[(size_t)s][(size_t)set] && !stages[(size_t)s].termsPending(set * kMaxCh))
            {
                isPending[(size_t)s][(size_t)set] = false;
                appliedSerial[(size_t)s][(size_t)set] = pendingSerial[(size_t)s][(size_t)set];
            }
}

void TailConvolver::setIR(std::shared_ptr<const TailIR> ir) noexcept
{
    if (ir == nullptr)
        return;
    if (cur == nullptr)
    {
        cur = std::move(ir);
        assign(curSet, cur.get());
        return;
    }
    if (next == nullptr && fade < 0)
    {
        next = std::move(ir);
        assign(1 - curSet, next.get());
    }
    else
        queued = std::move(ir); // a newer one while fading: it follows right after
}

void TailConvolver::adoptLatest() noexcept
{
    // not sounding: take the newest response directly, no crossfade needed (the processor keeps the
    // old one alive)
    if (queued == nullptr && next == nullptr)
        return;
    if (queued != nullptr)
    {
        cur = std::move(queued);
        next.reset();
    }
    else
        cur = std::move(next);
    fade = -1;
    assign(curSet, cur.get());
    assign(1 - curSet, nullptr);
}

uint32_t TailConvolver::oldestSerialInUse() const noexcept
{
    uint32_t s = kNone;
    for (auto *p : {cur.get(), next.get(), queued.get()})
        if (p != nullptr)
            s = juce::jmin(s, p->serial);
    for (int st = 0; st < TailIR::kStages; ++st)
        for (int set = 0; set < 2; ++set)
        {
            s = juce::jmin(s, appliedSerial[(size_t)st][(size_t)set]);
            if (isPending[(size_t)st][(size_t)set])
                s = juce::jmin(s, pendingSerial[(size_t)st][(size_t)set]);
        }
    return s;
}

void TailConvolver::process(const float *in, float *const *out, int numOut, int n, float g0,
                            float g1) noexcept
{
    const int chans = juce::jmin(numOut, kMaxCh);
    const float inv = n > 0 ? 1.0f / (float)n : 0.0f;
    int done = 0;
    while (done < n)
    {
        int len = n - done;
        for (auto &st : stages)
            len = juce::jmin(len, st.toBoundary());
        if (fade >= 0)
            len = juce::jmin(len, kFade - fade);
        const int oc = curSet * kMaxCh, on = (1 - curSet) * kMaxCh;
        const int p0 = stages[0].position(), p1 = stages[1].position(), p2 = stages[2].position();
        for (int c = 0; c < chans; ++c)
        {
            const float *a0 = stages[0].output(oc + c) + p0;
            const float *a1 = stages[1].output(oc + c) + p1;
            const float *a2 = stages[2].output(oc + c) + p2;
            float *o = out[c] + done;
            if (fade >= 0)
            {
                const float *b0 = stages[0].output(on + c) + p0;
                const float *b1 = stages[1].output(on + c) + p1;
                const float *b2 = stages[2].output(on + c) + p2;
                for (int i = 0; i < len; ++i)
                {
                    const float g = g0 + (g1 - g0) * (float)(done + i + 1) * inv;
                    const float f = (float)(fade + i) / (float)kFade;
                    const float a = a0[i] + a1[i] + a2[i], b = b0[i] + b1[i] + b2[i];
                    o[i] += (a + (b - a) * f) * g;
                }
            }
            else
                for (int i = 0; i < len; ++i)
                {
                    const float g = g0 + (g1 - g0) * (float)(done + i + 1) * inv;
                    o[i] += (a0[i] + a1[i] + a2[i]) * g;
                }
        }
        const float *ins[1]{in + done};
        for (auto &st : stages)
            st.push(ins, len);
        trackApplied();
        done += len;

        if (fade >= 0)
        {
            fade += len;
            if (fade >= kFade)
            {
                // the next response is the sounding one now; the old one's set is free (its spectra
                // are kept alive until no stage reads them any more: oldestSerialInUse)
                curSet = 1 - curSet;
                cur = next; // the old one is released here: the processor keeps a copy (freed on
                            // its thread)
                next = queued;
                queued = nullptr;
                fade = -1;
                assign(1 - curSet, next.get());
            }
        }
        else if (next != nullptr)
        {
            // crossfade to the new response once every stage has computed it once
            bool ready = true;
            for (auto &st : stages)
                ready = ready && st.blocksWithTerms((1 - curSet) * kMaxCh) >= 1;
            if (ready)
                fade = 0;
        }
    }
}
} // namespace rysum
