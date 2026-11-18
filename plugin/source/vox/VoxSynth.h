// Vox humana - the organ synth of NeuralRysum v2 (neuralrysum/synth.py), frame by frame.
//
// One frame = 128 samples at 48 kHz. Per voice and frame a small MLP turns
// (key, gate, onset / release timers, timbre) into 511 harmonic amplitudes of key/2 and
// 32 noise-band gains. The harmonics become one 1024-sample wavetable per voice and frame
// (inverse FFT); the oscillator crossfades from the previous frame's table to the new one
// over the frame. Then band noise and, optionally, the learned room (2 s impulse response).
// Real-time safe after load().
#pragma once

#include "VoxFFT.h"
#include "VoxModel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vox
{
struct VoiceState
{
    float key = 0.0f;     // fractional MIDI note, <= 0 = unused
    float gate = 0.0f;    // 1 while held
    float onAge = 0.0f;   // seconds since (re)start
    float offAge = -1.0f; // seconds since release, -1 while held
};

/** uniformly partitioned overlap-save convolution, block = frame, no latency */
class FrameConvolver
{
public:
    void init(const std::vector<float> &ir, int block)
    {
        B = block;
        N = 2 * B;
        fft.init(N);
        parts = (int)((ir.size() + (size_t)B - 1) / (size_t)B);
        H.assign((size_t)parts * N);
        used.assign((size_t)parts, 0);
        Floats seg((size_t)N);
        for (int p = 0; p < parts; ++p)
        {
            seg.clear();
            for (int i = 0; i < B; ++i)
            {
                const size_t s = (size_t)p * B + (size_t)i;
                if (s < ir.size())
                {
                    seg[(size_t)i] = ir[s];
                    used[(size_t)p] = used[(size_t)p] || ir[s] != 0.0f;
                }
            }
            fft.forward(seg.data(), H.data() + (size_t)p * N);
        }
        fdl.assign((size_t)parts * N);
        win.assign((size_t)N);
        acc.assign((size_t)N);
        out.assign((size_t)N);
        reset();
    }
    void reset() noexcept
    {
        fdl.clear();
        win.clear();
        pos = 0;
    }
    /** x: B new samples -> y: B samples of x convolved with the response */
    void process(const float *x, float *y) noexcept
    {
        std::memmove(win.data(), win.data() + B, (size_t)B * sizeof(float));
        std::memcpy(win.data() + B, x, (size_t)B * sizeof(float));
        pos = (pos + parts - 1) % parts;
        float *X = fdl.data() + (size_t)pos * N;
        fft.forward(win.data(), X);
        acc.clear();
        for (int p = 0; p < parts; ++p)
        {
            if (!used[(size_t)p])
                continue; // (the tail's first 80 ms: nothing to add)
            const float *Xp = fdl.data() + (size_t)((pos + p) % parts) * N;
            const float *Hp = H.data() + (size_t)p * N;
            acc[0] += Xp[0] * Hp[0];
            acc[1] += Xp[1] * Hp[1];
            for (int k = 2; k < N; k += 2)
            {
                const float xr = Xp[k], xi = Xp[k + 1], hr = Hp[k], hi = Hp[k + 1];
                acc[(size_t)k] += xr * hr - xi * hi;
                acc[(size_t)k + 1] += xr * hi + xi * hr;
            }
        }
        fft.inverse(acc.data(), out.data());
        const float sc = 1.0f / (float)N;
        for (int i = 0; i < B; ++i)
            y[i] = out[(size_t)(B + i)] * sc;
    }

private:
    int B = 128, N = 256, parts = 0, pos = 0;
    std::vector<char> used; // partitions that are not all zero
    FFT fft;
    Floats H, fdl, win, acc, out;
};

/** 32 log-spaced noise bands (50 Hz .. 22 kHz), each white noise through a 4th-order band-pass,
    scaled to unit power, / sqrt(bands) - the same statistics as synth.band_noise */
class NoiseBank
{
public:
    void init(int bands, double sr, uint32_t seed = 0x9E3779B9u)
    {
        for (int i = 0; i < 4; ++i)
            rng[(size_t)i] = (seed ^ (0x7F4A7C15u * (uint32_t)(i + 1))) | 1u;
        nb = bands;
        filt.assign((size_t)nb, {});
        for (int b = 0; b < nb; ++b)
        {
            const double lo = 50.0 * std::pow(22000.0 / 50.0, (double)b / nb);
            const double hi = 50.0 * std::pow(22000.0 / 50.0, (double)(b + 1) / nb);
            auto &f = filt[(size_t)b];
            f.s[0].highpass(lo, sr);
            f.s[1].highpass(lo, sr);
            f.s[2].lowpass(std::min(hi, 0.49 * sr), sr);
            f.s[3].lowpass(std::min(hi, 0.49 * sr), sr);
            f.scale = 1.0f;
        }
        // measure each band's level on white noise once
        std::vector<double> pw((size_t)nb, 0.0);
        const int len = (int)sr;
        for (int i = 0; i < len; ++i)
            for (int b = 0; b < nb; ++b)
            {
                const float v = filt[(size_t)b].tick(white(b));
                if (i > len / 4)
                    pw[(size_t)b] += (double)v * v;
            }
        for (int b = 0; b < nb; ++b)
        {
            const double rms = std::sqrt(pw[(size_t)b] / (len - len / 4 - 1));
            filt[(size_t)b].scale = (float)(1.0 / std::max(1e-9, rms) / std::sqrt((double)nb));
        }
        reset();
    }
    void reset() noexcept
    {
        for (auto &f : filt)
            for (auto &s : f.s)
                s.z1 = s.z2 = 0.0f;
    }
    int bands() const noexcept { return nb; }
    /** adds sum_b band_b(t) * gain_b(t) over n samples, gain glides linearly from g0 to g1 */
    void render(const float *g0, const float *g1, float *y, int n) noexcept
    {
        const float inv = 1.0f / (float)n;
        for (int b = 0; b < nb; ++b)
        {
            const float a = g0[b], d = (g1[b] - g0[b]) * inv;
            auto &f = filt[(size_t)b];
            if (a < 1e-7f && g1[b] < 1e-7f)
                continue; // silent band: the filter rests (it holds only noise, so nothing clicks)
            for (int i = 0; i < n; ++i)
                y[i] += f.tick(white(b)) * f.scale * (a + d * (float)i);
        }
    }

private:
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void set(double c0, double c1, double c2, double d0, double d1, double d2)
        {
            b0 = (float)(c0 / d0);
            b1 = (float)(c1 / d0);
            b2 = (float)(c2 / d0);
            a1 = (float)(d1 / d0);
            a2 = (float)(d2 / d0);
        }
        void lowpass(double fc, double sr)
        {
            const double w = 2.0 * 3.14159265358979 * fc / sr, c = std::cos(w), al = std::sin(w) / std::sqrt(2.0);
            set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
        }
        void highpass(double fc, double sr)
        {
            const double w = 2.0 * 3.14159265358979 * fc / sr, c = std::cos(w), al = std::sin(w) / std::sqrt(2.0);
            set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
        }
        float tick(float x) noexcept
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    struct Band
    {
        Biquad s[4];
        float scale = 1.0f;
        float tick(float x) noexcept { return s[3].tick(s[2].tick(s[1].tick(s[0].tick(x)))); }
    };
    float white(int b) noexcept
    {
        uint32_t &r = rng[(size_t)(b & 3)];
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        return ((float)r * (1.0f / 4294967296.0f) - 0.5f) * 3.4641016f; // unit variance
    }
    int nb = 32;
    std::vector<Band> filt;
    std::array<uint32_t, 4> rng{{0x9E3779B9u, 0x7F4A7C15u, 0x2545F491u, 0x6C8E9CF5u}};
};

class OrganSynth
{
public:
    static constexpr int kMaxVoices = 16; // 8 tracked + 8 played (MIDI); each runs on its own

    void load(const ModelFile &m)
    {
        sr = m.num("sample_rate");
        hop = m.num("hop");
        N = m.num("table");
        nh = N / 2 - 1;
        keyLo = m.num("key_lo");
        keyHi = m.num("key_hi");
        keyEmb = m.num("key_emb");
        nNoise = m.num("noise_bands");
        condDim = m.num("z_dim") + m.num("n_desc");
        const auto &consts = m.t("consts").data;
        releaseHold = consts[0];
        emb = m.t("synth.voice.emb").data;
        l1.init(m.t("synth.voice.l1.weight"), m.t("synth.voice.l1.bias"));
        l2.init(m.t("synth.voice.l2.weight"), m.t("synth.voice.l2.bias"));
        l3.init(m.t("synth.voice.l3.weight"), m.t("synth.voice.l3.bias"));
        lo.init(m.t("synth.voice.out.weight"), m.t("synth.voice.out.bias"));
        if (l1.in != keyEmb + 1 + 8 + 1 + 5 + 4 + 1 + condDim || lo.out != nh + nNoise)
            throw std::runtime_error("voice net does not match the configuration");
        const auto &ph = m.t("synth.phases").data;
        cr.resize((size_t)nh);
        ci.resize((size_t)nh);
        for (int k = 0; k < nh; ++k)
        {
            const float p = ph[(size_t)k] - 3.14159265358979f / 2.0f;
            cr[(size_t)k] = 0.5f * std::cos(p);
            ci[(size_t)k] = 0.5f * std::sin(p);
        }
        ir = m.t("synth.ir").data;
        // the learned room in two parts: its first 80 ms with every pipe (that is where the body of
        // the sound is: it moves with the pipe), the rest once on the sum (the room around them)
        {
            const size_t split = std::min(ir.size(), (size_t)kEarlySamples);
            std::vector<float> early(ir.begin(), ir.begin() + (std::ptrdiff_t)split), late(ir);
            std::fill(late.begin(), late.begin() + (std::ptrdiff_t)split, 0.0f);
            for (auto &c : earlyP)
                c.init(early, hop);
            tailConv.init(late, hop);
        }
        fft.init(N);
        spec.assign((size_t)N);
        for (auto &t : tables)
            t.assign((size_t)N);
        for (auto &t : prevTables)
            t.assign((size_t)N);
        featM.assign((size_t)kMaxVoices * l1.in, 0.0f);
        h1.assign((size_t)kMaxVoices * l1.outP, 0.0f);
        h2.assign((size_t)kMaxVoices * l1.outP, 0.0f);
        oM.assign((size_t)kMaxVoices * lo.outP, 0.0f);
        for (int p = 0; p < kPipes; ++p)
        {
            gNowP[(size_t)p].assign((size_t)nNoise, 0.0f);
            gPrevP[(size_t)p].assign((size_t)nNoise, 0.0f);
            dryP[(size_t)p].assign((size_t)hop, 0.0f);
            noiseP[(size_t)p].init(nNoise, (double)sr, 0x9E3779B9u * (uint32_t)(p + 1));
        }
        drySum.assign((size_t)hop, 0.0f);
        wet.assign((size_t)hop, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        for (auto &v : vs)
            v = Voice{};
        for (auto &t : prevTables)
            t.clear();
        for (auto &g : gPrevP)
            std::fill(g.begin(), g.end(), 0.0f);
        for (auto &nb : noiseP)
            nb.reset();
        for (auto &c : earlyP)
            c.reset();
        tailConv.reset();
        clearDisplay();
        first = true;
    }

    /** the stop's pipes: voice v sounds from pipe v % kPipes (tracked voice i and played voice i share
        pipe i, as in the 3D view); outputs kPipes pipes + 1 for the late part of the learned room */
    static constexpr int kPipes = 8;
    static constexpr int kEarlySamples = 3840; // 80 ms: the learned room's part that moves with a pipe

    int hopSize() const noexcept { return hop; }
    int sampleRate() const noexcept { return sr; }
    float releaseHoldSeconds() const noexcept { return releaseHold; }
    int conditioningSize() const noexcept { return condDim; }

    /** what each voice does now, for displays (written per frame on the audio thread, read anywhere):
        its key, its level, and the strength of eight bands of its harmonics on key/2 - 16', 8',
        5 1/3', 4', 3 1/5'-2 2/3', 2 2/7'-2', 1 7/9'-1', higher - each 0..1 relative to the strongest */
    struct Display
    {
        std::atomic<float> key{0.0f}, level{0.0f};
        std::array<std::atomic<float>, 8> rings{};
    };
    const Display &display(int v) const noexcept { return disp[(size_t)v]; }
    void clearDisplay() noexcept
    {
        for (auto &d : disp)
            d.level.store(0.0f, std::memory_order_relaxed);
    }

    bool noiseOn = true;   // band noise (off for exact tests)
    float roomMix = 1.0f;  // learned room: 0 = dry, 1 = as trained (dry + convolution)

    /** one frame: voices (nv <= kMaxVoices) as they are at the frame start, timbre cond -> hop samples.
        out = everything; pipes (optional, kPipes + 1 buffers of hop): every pipe, then the room's tail */
    void renderFrame(const VoiceState *voices, int nv, const float *cond, float *out,
                     float *const *pipes = nullptr) noexcept
    {
        nv = std::min(nv, kMaxVoices);
        for (auto &d : dryP)
            std::fill(d.begin(), d.end(), 0.0f);
        for (auto &g : gNowP)
            std::fill(g.begin(), g.end(), 0.0f);
        // 1. the network, for all sounding voices at once
        int nAlive = 0;
        std::array<int, kMaxVoices> row{};
        for (int v = 0; v < kMaxVoices; ++v)
        {
            const VoiceState in = v < nv ? voices[v] : VoiceState{};
            const bool used = in.key > 0.0f;
            const bool alive = used && (in.gate > 0.0f || (in.offAge >= 0.0f && in.offAge < releaseHold));
            row[(size_t)v] = alive ? nAlive : -1;
            if (alive)
                features(in.key, in, cond, featM.data() + (size_t)nAlive++ * l1.in);
        }
        if (nAlive > 0)
            runNet(nAlive);
        // 2. wavetables and oscillators
        for (int v = 0; v < kMaxVoices; ++v)
        {
            Voice &st = vs[(size_t)v];
            const VoiceState in = v < nv ? voices[v] : VoiceState{};
            const float key = in.key > 0.0f ? in.key : 60.0f;
            const float fBase = 440.0f * std::exp2((key - 69.0f) / 12.0f) / 2.0f;
            if (first)
                st.fPrev = fBase;
            float *cur = tables[(size_t)v].data();
            bool curOn = false;
            if (row[(size_t)v] >= 0)
            {
                const float *ov = oM.data() + (size_t)row[(size_t)v] * lo.outP;
                std::array<float, 8> bands{};
                buildTable(fBase, ov, cur, bands.data());
                curOn = true;
                auto &gp = gNowP[(size_t)(v % kPipes)];
                for (int b = 0; b < nNoise; ++b)
                    gp[(size_t)b] += std::exp(std::min(4.0f, std::max(-30.0f, ov[nh + b])));
                float total = 0.0f, top = 1.0e-20f;
                for (float e : bands)
                {
                    total += e;
                    top = std::max(top, e);
                }
                auto &d = disp[(size_t)v];
                d.key.store(key, std::memory_order_relaxed);
                d.level.store(std::sqrt(total), std::memory_order_relaxed);
                for (size_t j = 0; j < bands.size(); ++j)
                    d.rings[j].store(std::sqrt(bands[j] / top), std::memory_order_relaxed);
            }
            else
                disp[(size_t)v].level.store(0.0f, std::memory_order_relaxed);
            oscillate(st, fBase, st.prevOn ? prevTables[(size_t)v].data() : nullptr, curOn ? cur : nullptr,
                      dryP[(size_t)(v % kPipes)].data());
            if (curOn)
                std::memcpy(prevTables[(size_t)v].data(), cur, (size_t)N * sizeof(float));
            st.prevOn = curOn;
            st.fPrev = fBase;
        }
        first = false;
        // 3. noise, the learned room: every pipe its own, the room's tail from the sum
        std::fill(drySum.begin(), drySum.end(), 0.0f);
        std::fill(out, out + hop, 0.0f);
        for (int p = 0; p < kPipes; ++p)
        {
            auto &d = dryP[(size_t)p];
            if (noiseOn)
                noiseP[(size_t)p].render(gPrevP[(size_t)p].data(), gNowP[(size_t)p].data(), d.data(), hop);
            std::swap(gPrevP[(size_t)p], gNowP[(size_t)p]);
            for (int i = 0; i < hop; ++i)
                drySum[(size_t)i] += d[(size_t)i];
            float *po = pipes != nullptr ? pipes[p] : nullptr;
            if (roomMix > 0.0f)
                earlyP[(size_t)p].process(d.data(), wet.data());
            for (int i = 0; i < hop; ++i)
            {
                const float y = d[(size_t)i] + (roomMix > 0.0f ? roomMix * wet[(size_t)i] : 0.0f);
                out[i] += y;
                if (po != nullptr)
                    po[i] = y;
            }
        }
        float *to = pipes != nullptr ? pipes[kPipes] : nullptr;
        if (roomMix > 0.0f)
        {
            tailConv.process(drySum.data(), wet.data());
            for (int i = 0; i < hop; ++i)
            {
                const float y = roomMix * wet[(size_t)i];
                out[i] += y;
                if (to != nullptr)
                    to[i] = y;
            }
        }
        else if (to != nullptr)
            std::fill(to, to + hop, 0.0f);
    }

private:
    struct Voice
    {
        double cyc = 0.0; // phase in cycles
        float fPrev = 0.0f;
        bool prevOn = false;
    };

    const float *zeroG() noexcept
    {
        if (zeros.size() != (size_t)nNoise)
            zeros.assign((size_t)nNoise, 0.0f);
        return zeros.data();
    }

    void features(float key, const VoiceState &in, const float *cond, float *f) const noexcept
    {
        static constexpr float onTaus[5]{0.01f, 0.03f, 0.1f, 0.3f, 1.0f};
        static constexpr float offTaus[4]{0.01f, 0.03f, 0.1f, 0.3f};
        const float pos = std::min((float)(keyHi - keyLo), std::max(0.0f, key - (float)keyLo));
        const int lo0 = std::min(keyHi - keyLo - 1, (int)std::floor(pos));
        const float fr = pos - (float)lo0;
        const float *e0 = emb.data() + (size_t)lo0 * keyEmb, *e1 = e0 + keyEmb;
        for (int i = 0; i < keyEmb; ++i)
            *f++ = e0[i] * (1.0f - fr) + e1[i] * fr;
        const float p = (key - 60.0f) / 24.0f;
        *f++ = p;
        const float pi = 3.14159265358979f;
        for (int i = 0; i < 4; ++i)
            *f++ = std::sin(pi * p * (float)(1 << i));
        for (int i = 0; i < 4; ++i)
            *f++ = std::cos(pi * p * (float)(1 << i));
        *f++ = in.gate;
        for (float t : onTaus)
            *f++ = std::exp(-std::max(0.0f, in.onAge) / t);
        const float released = in.offAge >= 0.0f ? 1.0f : 0.0f;
        for (float t : offTaus)
            *f++ = std::exp(-std::max(0.0f, in.offAge) / t) * released;
        *f++ = released;
        for (int i = 0; i < condDim; ++i)
            *f++ = cond[i];
    }

    /** the MLP for n voices (rows of featM) -> rows of oM */
    void runNet(int n) noexcept
    {
        const int H = l1.outP;
        l1.applyBatch(featM.data(), n, h1.data());
        leaky(h1.data(), h1.data(), n * H);
        for (const Linear *l : {&l2, &l3})
        {
            l->applyBatch(h1.data(), n, h2.data());
            for (int i = 0; i < n * H; ++i)
            {
                const float v = h2[(size_t)i];
                h1[(size_t)i] += v > 0.0f ? v : kLeak * v;
            }
        }
        lo.applyBatch(h1.data(), n, oM.data());
    }

    /** harmonic k (1 = 16') -> display band 0..7 */
    static int bandOf(int k) noexcept
    {
        return k <= 4 ? k - 1 : (k <= 6 ? 4 : (k <= 8 ? 5 : (k <= 16 ? 6 : 7)));
    }

    void buildTable(float fBase, const float *o, float *table, float *bands) noexcept
    {
        spec.clear();
        const float lim = 0.45f * (float)sr, fade = 0.05f * (float)sr;
        for (int k = 1; k <= nh; ++k)
        {
            const float fk = (float)k * fBase;
            if (!(fk < lim))
                break;
            const float a = std::exp(std::min(4.0f, std::max(-30.0f, o[k - 1]))) *
                            std::min(1.0f, std::max(0.0f, (lim - fk) / fade));
            bands[bandOf(k)] += a * a;
            if (k < N / 2)
            {
                spec[(size_t)(2 * k)] = a * cr[(size_t)(k - 1)];
                spec[(size_t)(2 * k + 1)] = a * ci[(size_t)(k - 1)];
            }
        }
        fft.inverse(spec.data(), table);
    }

    void oscillate(Voice &st, float fBase, const float *prev, const float *cur, float *dry) noexcept
    {
        const double inv = 1.0 / (double)sr;
        const float fp = st.fPrev;
        const float invHop = 1.0f / (float)hop;
        for (int j = 0; j < hop; ++j)
        {
            const float w = (float)j * invHop;
            const float fs = fp * (1.0f - w) + fBase * w;
            st.cyc += (double)fs * inv;
            if (st.cyc >= 1.0)
                st.cyc -= 1.0; // fs < sr, so one step never covers a whole cycle
            if (prev == nullptr && cur == nullptr)
                continue;
            const float pos = (float)st.cyc * (float)N;
            const int i0 = std::min((int)pos, N - 1), i1 = (i0 + 1) & (N - 1);
            const float fl = (float)i0;
            const float a = pos - fl;
            float y = 0.0f;
            if (prev != nullptr)
                y += (prev[i0] * (1.0f - a) + prev[i1] * a) * (1.0f - w);
            if (cur != nullptr)
                y += (cur[i0] * (1.0f - a) + cur[i1] * a) * w;
            dry[j] += y;
        }
    }

    int sr = 48000, hop = 128, N = 1024, nh = 511, keyLo = 21, keyHi = 108, keyEmb = 16, nNoise = 32,
        condDim = 11;
    float releaseHold = 0.5f;
    std::vector<float> emb, cr, ci, ir;
    Linear l1, l2, l3, lo;
    FFT fft;
    Floats spec;
    std::array<Floats, kMaxVoices> tables, prevTables;
    std::array<Voice, kMaxVoices> vs{};
    std::array<Display, kMaxVoices> disp{};
    std::vector<float> featM, h1, h2, oM, drySum, wet, zeros;
    std::array<std::vector<float>, kPipes> dryP, gNowP, gPrevP;
    std::array<NoiseBank, kPipes> noiseP;
    std::array<FrameConvolver, kPipes> earlyP;
    FrameConvolver tailConv;
    bool first = true;
};
} // namespace vox
