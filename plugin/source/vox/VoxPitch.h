// Vox humana - polyphonic pitch estimator and voice tracker (NeuralRysum v2, neuralrysum/pitch.py).
//
// Streaming, one frame per 64 input samples at 48 kHz:
//   push(64 samples) -> causal multi-window log spectrum -> harmonic stack -> causal 2-D CNN
//   -> salience / onset logits (n_bins) -> VoiceTracker::step -> up to 8 voices.
// Real-time safe after load(): no allocation, no locks.
#pragma once

#include "VoxFFT.h"
#include "VoxModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "VoxSimd.h"

namespace vox
{
// ---- 4-wide vector helpers ------------------------------------------------------------------
#if VOX_SSE
using V4 = __m128;
inline V4 v4load(const float *p) noexcept { return _mm_loadu_ps(p); }
inline void v4store(float *p, V4 v) noexcept { _mm_storeu_ps(p, v); }
inline V4 v4set(float s) noexcept { return _mm_set1_ps(s); }
inline V4 v4mac(V4 acc, V4 a, V4 b) noexcept { return _mm_add_ps(acc, _mm_mul_ps(a, b)); }
#elif VOX_NEON
using V4 = float32x4_t;
inline V4 v4load(const float *p) noexcept { return vld1q_f32(p); }
inline void v4store(float *p, V4 v) noexcept { vst1q_f32(p, v); }
inline V4 v4set(float s) noexcept { return vdupq_n_f32(s); }
inline V4 v4mac(V4 acc, V4 a, V4 b) noexcept { return vmlaq_f32(acc, a, b); }
#else
struct V4
{
    float v[4];
};
inline V4 v4load(const float *p) noexcept { return {{p[0], p[1], p[2], p[3]}}; }
inline void v4store(float *p, V4 a) noexcept { std::memcpy(p, a.v, sizeof a.v); }
inline V4 v4set(float s) noexcept { return {{s, s, s, s}}; }
inline V4 v4mac(V4 acc, V4 a, V4 b) noexcept
{
    for (int i = 0; i < 4; ++i)
        acc.v[i] += a.v[i] * b.v[i];
    return acc;
}
#endif

#if VOX_SSE
/** the conv's inner loops, 8 bins x 2 x 4 output channels per step, with FMA */
VOX_AVX2_FN inline void convAvx2(const float *wg, int kt, int cin, int kf, int df, int pf, int margin,
                                 const float *const *in, int inStride, const float *bias4, int nco,
                                 float *out, int outStride, int nbp) noexcept
{
    for (int b0 = 0; b0 < nbp; b0 += 16)
    {
        __m256 a0[4], a1[4];
        for (int j = 0; j < 4; ++j)
        {
            a0[j] = _mm256_set1_ps(bias4[j]);
            a1[j] = a0[j];
        }
        const float *wp = wg;
        for (int t = 0; t < kt; ++t)
        {
            const float *frame = in[t];
            for (int ci = 0; ci < cin; ++ci)
            {
                const float *row = frame + (size_t)ci * inStride + margin - pf + b0;
                for (int f = 0; f < kf; ++f, wp += 4)
                {
                    const float *x = row + f * df;
                    const __m256 x0 = _mm256_loadu_ps(x), x1 = _mm256_loadu_ps(x + 8);
                    for (int j = 0; j < 4; ++j)
                    {
                        const __m256 wj = _mm256_broadcast_ss(wp + j);
                        a0[j] = _mm256_fmadd_ps(wj, x0, a0[j]);
                        a1[j] = _mm256_fmadd_ps(wj, x1, a1[j]);
                    }
                }
            }
        }
        for (int j = 0; j < nco; ++j)
        {
            float *o = out + (size_t)j * outStride + margin + b0;
            _mm256_storeu_ps(o, a0[j]);
            _mm256_storeu_ps(o + 8, a1[j]);
        }
    }
    _mm256_zeroupper();
}
#endif

/** One causal 2-D conv layer over (channel, bin) frames.
    Frames are stored channel by channel, each channel row `stride` floats wide with `kMargin`
    zeros on the left: bin b of channel c is at row(c)[kMargin + b]. */
struct Conv2dLayer
{
    static constexpr int kMargin = 20;
    int cin = 0, cout = 0, kf = 0, kt = 0, df = 1, dt = 1, pf = 0;
    std::vector<float> w; // [co/4][kt][ci][kf][4]
    std::vector<float> bias;

    void init(const Tensor &W, const Tensor &B, int dilT, int dilF)
    {
        if (W.shape.size() != 4)
            throw std::runtime_error("conv2d weight must be 4-D");
        cout = W.shape[0];
        cin = W.shape[1];
        kf = W.shape[2];
        kt = W.shape[3];
        dt = dilT;
        df = dilF;
        pf = (kf - 1) * df / 2;
        if (pf > kMargin)
            throw std::runtime_error("conv2d frequency kernel wider than the margin");
        const int groups = (cout + 3) / 4;
        w.assign((size_t)groups * kt * cin * kf * 4, 0.0f);
        for (int co = 0; co < cout; ++co)
            for (int ci = 0; ci < cin; ++ci)
                for (int f = 0; f < kf; ++f)
                    for (int t = 0; t < kt; ++t)
                        w[((((size_t)(co / 4) * kt + t) * cin + ci) * kf + f) * 4 + (size_t)(co % 4)] =
                            W.data[(((size_t)co * cin + ci) * kf + f) * kt + t];
        bias = B.data;
    }

    /** out rows: cout channels at `outStride`; in[t] = the input frame for time tap t
        (tap kt-1 = current frame, tap kt-1-j = j*dt frames back). nbp = bins computed (multiple of 4). */
    void run(const float *const *in, int inStride, float *out, int outStride, int nbp) const noexcept
    {
        const int groups = (cout + 3) / 4;
        for (int g = 0; g < groups; ++g)
        {
            const int nco = std::min(4, cout - 4 * g);
            const float *wg = w.data() + (size_t)g * kt * cin * kf * 4;
#if VOX_SSE
            if (haveAvx2() && nbp % 16 == 0)
            {
                float b4[4]{};
                for (int j = 0; j < nco; ++j)
                    b4[j] = bias[(size_t)(4 * g + j)];
                convAvx2(wg, kt, cin, kf, df, pf, kMargin, in, inStride, b4, nco,
                         out + (size_t)(4 * g) * outStride, outStride, nbp);
                continue;
            }
#endif
            for (int b0 = 0; b0 < nbp; b0 += 8)
            {
                V4 a0[4], a1[4];
                for (int j = 0; j < 4; ++j)
                {
                    const float bj = j < nco ? bias[(size_t)(4 * g + j)] : 0.0f;
                    a0[j] = v4set(bj);
                    a1[j] = v4set(bj);
                }
                const bool two = b0 + 4 < nbp;
                const float *wp = wg;
                for (int t = 0; t < kt; ++t)
                {
                    const float *frame = in[t];
                    for (int ci = 0; ci < cin; ++ci)
                    {
                        const float *row = frame + (size_t)ci * inStride + kMargin - pf + b0;
                        for (int f = 0; f < kf; ++f, wp += 4)
                        {
                            const float *x = row + f * df;
                            const V4 x0 = v4load(x);
                            const V4 x1 = two ? v4load(x + 4) : x0;
                            for (int j = 0; j < 4; ++j)
                            {
                                const V4 wj = v4set(wp[j]);
                                a0[j] = v4mac(a0[j], wj, x0);
                                a1[j] = v4mac(a1[j], wj, x1);
                            }
                        }
                    }
                }
                for (int j = 0; j < nco; ++j)
                {
                    float *o = out + (size_t)(4 * g + j) * outStride + kMargin + b0;
                    v4store(o, a0[j]);
                    if (two)
                        v4store(o + 4, a1[j]);
                }
            }
        }
    }
};

/** frames of the past, newest last: at(0) = current, at(d) = d frames back */
struct FrameRing
{
    int depth = 0, size = 0, pos = 0;
    std::vector<float> buf;
    void init(int frames, int floatsPerFrame)
    {
        depth = frames;
        size = floatsPerFrame;
        buf.assign((size_t)frames * (size_t)floatsPerFrame, 0.0f);
        pos = 0;
    }
    void reset() noexcept
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        pos = 0;
    }
    float *next() noexcept // advance and return the slot of the new current frame
    {
        pos = (pos + 1) % depth;
        return buf.data() + (size_t)pos * (size_t)size;
    }
    const float *at(int back) const noexcept
    {
        return buf.data() + (size_t)((pos - back + depth * 64) % depth) * (size_t)size;
    }
};

class PitchEstimator
{
public:
    void load(const ModelFile &m)
    {
        hop = m.num("est_hop");
        nb = m.num("n_bins");
        nIn = m.num("n_in");
        loPad = m.num("lo_pad");
        bps = m.num("bins_per_semitone");
        midiLo = m.num("midi_lo");
        shifts = m.list("shifts");
        windows = m.list("windows");
        const int nw = (int)windows.size();
        maxWin = *std::max_element(windows.begin(), windows.end());
        win.assign((size_t)nw, {});
        idx.assign((size_t)nw, {});
        frac.assign((size_t)nw, {});
        dst.assign((size_t)nw, {});
        winScale.assign((size_t)nw, 0.0f);
        ffts.clear();
        ffts.resize((size_t)nw);
        for (int j = 0; j < nw; ++j)
        {
            const std::string s = "pitch.spec.";
            win[(size_t)j] = m.t(s + "win" + std::to_string(j)).data;
            float sum = 0.0f;
            for (float v : win[(size_t)j])
                sum += v;
            winScale[(size_t)j] = 2.0f / sum;
            if (m.has(s + "idx" + std::to_string(j)))
            {
                for (float v : m.t(s + "idx" + std::to_string(j)).data)
                    idx[(size_t)j].push_back((int)v);
                frac[(size_t)j] = m.t(s + "frac" + std::to_string(j)).data;
                for (float v : m.t(s + "dst" + std::to_string(j)).data)
                    dst[(size_t)j].push_back((int)v);
            }
            ffts[(size_t)j].init(windows[(size_t)j]);
        }
        frameBuf.assign((size_t)maxWin);
        spec.assign((size_t)maxWin);
        hist.assign((size_t)maxWin, 0.0f);
        logSpec.assign((size_t)nIn, 0.0f);
        lin.assign((size_t)nIn, 0.0f);

        l1.init(m.t("pitch.l1.conv.weight"), m.t("pitch.l1.conv.bias"), 1, 1);
        l2.init(m.t("pitch.l2.conv.weight"), m.t("pitch.l2.conv.bias"), 2, 1);
        l3.init(m.t("pitch.l3.conv.weight"), m.t("pitch.l3.conv.bias"), 4, 1);
        l4.init(m.t("pitch.l4.conv.weight"), m.t("pitch.l4.conv.bias"), 1, 3);
        const Tensor &ow = m.t("pitch.out.weight");
        outW = ow.data; // (2, cOut, 1, 1)
        outB = m.t("pitch.out.bias").data;
        cA = l1.cout;
        cB = l4.cout;
        if ((int)ow.shape[1] != cA + cB)
            throw std::runtime_error("pitch out layer does not match");
        nbp = (nb + 15) / 16 * 16;
        stride = Conv2dLayer::kMargin + nbp + Conv2dLayer::kMargin;
        const int H = (int)shifts.size();
        ringH.init(1 + (l1.kt - 1) * l1.dt, H * stride);
        ringA1.init(1 + (l2.kt - 1) * l2.dt, cA * stride);
        ringA2.init(1 + (l3.kt - 1) * l3.dt, cA * stride);
        tmp.assign((size_t)cA * stride, 0.0f);
        a3.assign((size_t)cA * stride, 0.0f);
        bOut.assign((size_t)cB * stride, 0.0f);
        sal.assign((size_t)nb, 0.0f);
        ons.assign((size_t)nb, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        std::fill(hist.begin(), hist.end(), 0.0f);
        ringH.reset();
        ringA1.reset();
        ringA2.reset();
        std::fill(sal.begin(), sal.end(), 0.0f);
        std::fill(ons.begin(), ons.end(), 0.0f);
    }

    int hopSize() const noexcept { return hop; }
    int longestWindow() const noexcept { return maxWin; }
    int numBins() const noexcept { return nb; }
    double binMidi(int i) const noexcept { return midiLo + (double)i / (double)bps; }

    /** one frame: `hop` new samples. Afterwards salience() / onset() hold the logits. */
    void push(const float *x) noexcept
    {
        std::memmove(hist.data(), hist.data() + hop, (size_t)(maxWin - hop) * sizeof(float));
        std::memcpy(hist.data() + (maxWin - hop), x, (size_t)hop * sizeof(float));
        spectrum();
        network();
    }

    const float *salienceLogits() const noexcept { return sal.data(); }
    const float *onsetLogits() const noexcept { return ons.data(); }

    /** how strong a note is in the last frame: the power of its harmonics 1 .. 7 (the ones the
        network stacks), each the strongest of three neighbouring bins, amplitude² (a sine of
        amplitude a: a²). Harmonics a chord shares count for every note that has them. */
    /** the input's power over the last `n` samples (Hann-weighted), on the same scale as
        harmonicPower (a sine of amplitude a: a²). Short: right at a note's start, when the long
        windows of the spectrum hold only a little of it */
    float shortPower(int n = 512) const noexcept
    {
        n = std::min(n, maxWin);
        const float *x = hist.data() + (maxWin - n);
        double s = 0.0, w2 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(6.283185307179586 * (i + 0.5) / n);
            s += w * w * (double)x[i] * x[i];
            w2 += w * w;
        }
        return (float)(2.0 * s / std::max(1e-12, w2));
    }

    float harmonicPower(double midi) const noexcept
    {
        const int b = (int)std::lround((midi - (double)midiLo) * (double)bps);
        float s = 0.0f;
        for (int sh : shifts)
        {
            if (sh < 0)
                continue; // (the subharmonic: not this note's)
            const int i = loPad + sh + b;
            float m = 0.0f;
            for (int d = -1; d <= 1; ++d)
                if (i + d >= 0 && i + d < nIn)
                    m = std::max(m, lin[(size_t)(i + d)]);
            s += m * m;
        }
        return s;
    }

private:
    void spectrum() noexcept
    {
        std::fill(logSpec.begin(), logSpec.end(), 0.0f);
        for (size_t j = 0; j < windows.size(); ++j)
        {
            if (dst[j].empty())
                continue;
            const int n = windows[j];
            const float *src = hist.data() + (maxWin - n);
            const float *w = win[j].data();
            for (int i = 0; i < n; ++i)
                frameBuf[(size_t)i] = src[i] * w[i];
            ffts[j].forward(frameBuf.data(), spec.data());
            const float sc = winScale[j];
            auto mag = [&](int k) -> float
            {
                if (k == 0)
                    return std::fabs(spec[0]) * sc;
                if (k == n / 2)
                    return std::fabs(spec[1]) * sc;
                const float re = spec[(size_t)(2 * k)], im = spec[(size_t)(2 * k + 1)];
                return std::sqrt(re * re + im * im) * sc;
            };
            const auto &id = idx[j];
            const auto &fr = frac[j];
            const auto &ds = dst[j];
            for (size_t q = 0; q < ds.size(); ++q)
            {
                const float f = fr[q];
                logSpec[(size_t)ds[q]] = mag(id[q]) * (1.0f - f) + mag(id[q] + 1) * f;
            }
        }
        std::copy(logSpec.begin(), logSpec.end(), lin.begin()); // (magnitudes, for harmonicPower)
        for (auto &v : logSpec)
        {
            const float db = 20.0f * std::log10(v + 1e-5f);
            v = std::min(1.0f, std::max(0.0f, (db + 100.0f) / 100.0f));
        }
    }

    void network() noexcept
    {
        const int M = Conv2dLayer::kMargin;
        // harmonic stack -> current frame of ringH
        float *h = ringH.next();
        for (size_t c = 0; c < shifts.size(); ++c)
        {
            float *row = h + c * (size_t)stride;
            const float *s = logSpec.data() + loPad + shifts[c];
            std::memcpy(row + M, s, (size_t)nb * sizeof(float));
        }
        auto taps = [](const FrameRing &r, const Conv2dLayer &l, const float **out)
        {
            for (int t = 0; t < l.kt; ++t)
                out[t] = r.at((l.kt - 1 - t) * l.dt);
        };
        const float *in[8];
        // a1 = leaky(l1(h))
        float *a1 = ringA1.next();
        taps(ringH, l1, in);
        l1.run(in, stride, a1, stride, nbp);
        activate(a1, cA, false, nullptr);
        // a2 = a1 + leaky(l2(a1))
        taps(ringA1, l2, in);
        l2.run(in, stride, tmp.data(), stride, nbp);
        float *a2 = ringA2.next();
        activate(tmp.data(), cA, true, a1);
        std::memcpy(a2, tmp.data(), (size_t)cA * stride * sizeof(float));
        // a3 = a2 + leaky(l3(a2))
        taps(ringA2, l3, in);
        l3.run(in, stride, a3.data(), stride, nbp);
        activate(a3.data(), cA, true, a2);
        // b = leaky(l4(a3))
        const float *a3p = a3.data();
        l4.run(&a3p, stride, bOut.data(), stride, nbp);
        activate(bOut.data(), cB, false, nullptr);
        // out = 1x1 over cat(a3, b)
        for (int o = 0; o < 2; ++o)
        {
            float *dstv = o == 0 ? sal.data() : ons.data();
            const float *wo = outW.data() + (size_t)o * (cA + cB);
            for (int b = 0; b < nb; ++b)
                dstv[b] = outB[(size_t)o];
            for (int c = 0; c < cA + cB; ++c)
            {
                const float wc = wo[c];
                const float *row = (c < cA ? a3.data() + (size_t)c * stride : bOut.data() + (size_t)(c - cA) * stride) + M;
                for (int b = 0; b < nb; ++b)
                    dstv[b] += wc * row[b];
            }
        }
    }

    /** leaky ReLU on `ch` rows (+ residual), then zero everything outside bins 0 .. nb-1 */
    void activate(float *x, int ch, bool residual, const float *res) const noexcept
    {
        const int M = Conv2dLayer::kMargin;
        for (int c = 0; c < ch; ++c)
        {
            float *row = x + (size_t)c * stride;
            const float *r = residual ? res + (size_t)c * stride : nullptr;
            for (int b = 0; b < nb; ++b)
            {
                const float v = row[M + b];
                const float a = v > 0.0f ? v : kLeak * v;
                row[M + b] = residual ? r[M + b] + a : a;
            }
            std::fill(row, row + M, 0.0f);
            std::fill(row + M + nb, row + stride, 0.0f);
        }
    }

    int hop = 64, nb = 0, nIn = 0, loPad = 0, bps = 3, midiLo = 21, maxWin = 0;
    std::vector<int> shifts, windows;
    std::vector<std::vector<float>> win, frac;
    std::vector<std::vector<int>> idx, dst;
    std::vector<float> winScale;
    std::vector<FFT> ffts;
    Floats frameBuf, spec;
    std::vector<float> hist, logSpec, lin;

    Conv2dLayer l1, l2, l3, l4;
    std::vector<float> outW, outB;
    int cA = 0, cB = 0, nbp = 0, stride = 0;
    FrameRing ringH, ringA1, ringA2;
    std::vector<float> tmp, a3, bOut, sal, ons;
};

/** VoiceTracker of neuralrysum/pitch.py, step for step (same decisions on the same salience). */
class VoiceTracker
{
public:
    static constexpr int kMaxVoices = 8;
    struct Params
    {
        double on = 0.6, off = 0.3, match = 0.7, onset = 0.5;
        int hold = 24, confirm = 4;
        int voices = kMaxVoices; // new notes only start on the first `voices` voices
    };

    void init(int bins, double midiLo, int binsPerSemitone, int voices)
    {
        nb = bins;
        lo = midiLo;
        bps = binsPerSemitone;
        n = std::min(voices, kMaxVoices);
        binMidi.resize((size_t)nb);
        for (int i = 0; i < nb; ++i)
            binMidi[(size_t)i] = lo + (double)i / (double)bps;
        cand.reserve(256);
        pending.reserve(256);
        nextPending.reserve(256);
        reset();
    }

    void reset() noexcept
    {
        midi.fill(0.0);
        active.fill(false);
        missing.fill(0);
        age.fill(0);
        started.fill(false);
        pending.clear();
    }

    Params params;

    /** one frame of sigmoid salience / onset (nb values each) */
    void step(const float *sal, const float *ons) noexcept
    {
        started.fill(false);
        findPeaks(sal, params.off);
        used.assign(cand.size(), false);
        // oldest voices claim their peak first (stable order, like numpy's argsort for 8 values)
        std::array<int, kMaxVoices> order{};
        for (int v = 0; v < n; ++v)
            order[(size_t)v] = v;
        std::stable_sort(order.begin(), order.begin() + n, [&](int a, int b) { return age[(size_t)a] > age[(size_t)b]; });
        for (int oi = 0; oi < n; ++oi)
        {
            const int v = order[(size_t)oi];
            if (!active[(size_t)v])
                continue;
            int best = -1;
            double bd = params.match;
            for (size_t j = 0; j < cand.size(); ++j)
            {
                const double d = std::fabs(cand[j].m - midi[(size_t)v]);
                if (!used[j] && d < bd)
                {
                    best = (int)j;
                    bd = d;
                }
            }
            if (best < 0)
            {
                if (++missing[(size_t)v] > params.hold)
                    active[(size_t)v] = false;
                continue;
            }
            used[(size_t)best] = true;
            const double m = cand[(size_t)best].m;
            midi[(size_t)v] = m;
            missing[(size_t)v] = 0;
            const int k = (int)std::nearbyint((m - binMidi[0]) * 3.0);
            if (k >= 0 && k < nb && (double)ons[k] > params.onset && age[(size_t)v] > 6)
            {
                age[(size_t)v] = 0;
                started[(size_t)v] = true;
            }
            else
                ++age[(size_t)v];
        }
        nextPending.clear();
        for (size_t j = 0; j < cand.size(); ++j)
        {
            if (used[j] || cand[j].s < params.on)
                continue;
            const double m = cand[j].m;
            int seenBefore = 0;
            for (const auto &p : pending)
                if (std::fabs(p.m - m) < params.match)
                    seenBefore = std::max(seenBefore, p.count);
            const int seen = 1 + seenBefore;
            int free = -1;
            for (int v = 0; v < std::min(n, params.voices); ++v)
                if (!active[(size_t)v])
                {
                    free = v;
                    break;
                }
            if (seen < params.confirm || free < 0)
            {
                nextPending.push_back({m, seen});
                continue;
            }
            active[(size_t)free] = true;
            midi[(size_t)free] = m;
            missing[(size_t)free] = 0;
            age[(size_t)free] = 0;
            started[(size_t)free] = true;
        }
        pending.swap(nextPending);
    }

    int numVoices() const noexcept { return n; }
    double voiceMidi(int v) const noexcept { return midi[(size_t)v]; }
    bool voiceGate(int v) const noexcept { return active[(size_t)v]; }
    bool voiceStarted(int v) const noexcept { return started[(size_t)v]; }

private:
    struct Peak
    {
        double m;
        float s;
    };
    struct Pend
    {
        double m;
        int count;
    };

    void findPeaks(const float *s, double thresh) noexcept
    {
        cand.clear();
        for (int i = 1; i < nb - 1; ++i)
        {
            if ((double)s[i] >= thresh && s[i] >= s[i - 1] && s[i] > s[i + 1])
            {
                const float wsum = (s[i - 1] + s[i]) + s[i + 1];
                const double num = (double)s[i - 1] * binMidi[(size_t)(i - 1)] + (double)s[i] * binMidi[(size_t)i] +
                                   (double)s[i + 1] * binMidi[(size_t)(i + 1)];
                if (cand.size() < cand.capacity())
                    cand.push_back({num / (double)wsum, s[i]});
            }
        }
    }

    int nb = 0, bps = 3, n = 8;
    double lo = 21.0;
    std::vector<double> binMidi;
    std::array<double, kMaxVoices> midi{};
    std::array<bool, kMaxVoices> active{}, started{};
    std::array<int, kMaxVoices> missing{}, age{};
    std::vector<Peak> cand;
    std::vector<bool> used;
    std::vector<Pend> pending, nextPending;
};
} // namespace vox
