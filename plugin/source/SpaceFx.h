#pragma once

#include "Common.h"
#include "Spatial.h"
#include "SpatialDegrade.h"
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <vector>

namespace rysum
{
// ---------------------------------------------------------------------------
// Photographic operations on the whole sound field (Effekte page, tab "Stimmung & Belichtung") and the
// dispersive walls of the Saal ("Eis"). All memory is taken in prepare(); everything is off by default and
// then touches nothing.
// ---------------------------------------------------------------------------

/** "Doppelbelichtung": the sound field of a few seconds ago, turned and/or mirrored, faded, laid over the present
    (Ambisonics up to 3rd order is copied; stereo: L/R, mirrored = swapped) */
struct DoubleExposureParams
{
    float mix = 0.0f;          // 0 = off .. 1 = as loud as the present
    float time = 2.0f;         // seconds back
    float turn = 0.0f;         // degrees (around the vertical axis)
    int   mirror = 0;          // 0 none, 1 left-right, 2 front-back, 3 up-down
    float fade = 0.3f;         // 0 = as it was .. 1 = dull, faded (low-pass)
    float again = 0.0f;        // 0..0.85: the copy is exposed again (several images further back)
    bool on() const noexcept { return mix > 1.0e-4f; }
};

/** "Belichtung" (long exposure): every direction of the sphere keeps what passes through it and lets it go slowly -
    moving pipes leave trails; up to 3rd order (stereo: left / right) */
struct LongExposureParams
{
    float mix = 0.0f;          // 0 = off .. 1 = trails as loud as the sound
    float time = 4.0f;         // seconds until a trail has faded by 60 dB
    float bright = 0.5f;       // 0 = dark trails .. 1 = as bright as the sound
    bool on() const noexcept { return mix > 1.0e-4f; }
};

namespace spacefx
{
    constexpr int kMaxCh = 16;               // 3rd order
    inline int degreeOf (int acn) noexcept { return (int) std::floor (std::sqrt ((double) acn) + 1.0e-9); }
    inline int orderOf (int acn) noexcept { const int l = degreeOf (acn); return acn - l * l - l; }     // m

    /** sign of every channel when the field is mirrored (AmbiX: ACN / SN3D, Y_{l,m>0} ~ cos m phi, Y_{l,m<0} ~ sin |m| phi) */
    inline float mirrorSign (int acn, int mirror) noexcept
    {
        const int l = degreeOf (acn), m = orderOf (acn), am = std::abs (m);
        switch (mirror)
        {
            case 1: return m < 0 ? -1.0f : 1.0f;                                                     // left <-> right
            case 2: return m == 0 ? 1.0f : (m > 0 ? ((am & 1) ? -1.0f : 1.0f) : ((am & 1) ? 1.0f : -1.0f));   // front <-> back
            case 3: return ((l + am) & 1) ? -1.0f : 1.0f;                                              // up <-> down
            default: return 1.0f;
        }
    }
}

class DoubleExposure
{
public:
    void prepare (double sampleRate, int maxSeconds = 8)
    {
        sr = sampleRate;
        len = 1;
        while (len < (int) (maxSeconds * sr) + 8) len <<= 1;
        mask = (uint32_t) (len - 1);
        buf.assign ((size_t) spacefx::kMaxCh * (size_t) len, 0.0f);
        reset();
    }
    void reset() noexcept
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        lp.fill (0.0f); w = 0; dSm = -1.0f; gSm = 0.0f; wasOn = false;
    }

    /** ch: the output channels (stereo: 2, Ambisonics: (order+1)^2) - the copy is added in place */
    void process (float* const* ch, int numCh, bool stereo, int n, const DoubleExposureParams& p) noexcept
    {
        if (buf.empty() || n <= 0) return;
        const int K = juce::jmin (numCh, spacefx::kMaxCh);
        const bool on = p.on();
        if (! on && ! wasOn) return;
        if (on && ! wasOn) { std::fill (buf.begin(), buf.end(), 0.0f); lp.fill (0.0f); dSm = -1.0f; }
        wasOn = on || gSm > 1.0e-5f;
        const float dTarget = (float) juce::jlimit (64.0, (double) (len - n - 8), p.time * sr);
        if (dSm < 0.0f) dSm = dTarget;
        const float gTarget = on ? p.mix : 0.0f;
        const float lpA = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * juce::jmap (std::pow (p.fade, 0.6f), 18000.0f, 900.0f) / (float) sr);
        const float again = juce::jlimit (0.0f, 0.85f, p.again);
        // rotation (yaw) and mirror as a per-channel matrix: pairs (l, m) / (l, -m) turn by m * turn
        const float turn = juce::degreesToRadians (p.turn);
        std::array<float, spacefx::kMaxCh> sgn {};
        for (int c = 0; c < K; ++c) sgn[(size_t) c] = stereo ? 1.0f : spacefx::mirrorSign (c, p.mirror);
        const bool swapLR = stereo && (p.mirror == 1 || p.mirror == 2 || std::abs (std::remainder (p.turn, 360.0f)) > 90.0f);
        std::array<float, spacefx::kMaxCh> y {};
        for (int i = 0; i < n; ++i)
        {
            dSm += 0.0005f * (dTarget - dSm);        // a changed time glides (tape-like)
            gSm += 0.002f * (gTarget - gSm);
            const float d = dSm;
            const int di = (int) d;
            const float fd = d - (float) di;
            const uint32_t at = w - (uint32_t) di;
            for (int c = 0; c < K; ++c)
            {
                const float* b = buf.data() + (size_t) c * (size_t) len;
                const float a0 = b[at & mask], a1 = b[(at - 1u) & mask];
                y[(size_t) c] = a0 + fd * (a1 - a0);
            }
            // turn + mirror the old image
            if (! stereo)
            {
                std::array<float, spacefx::kMaxCh> r = y;
                if (std::abs (turn) > 1.0e-5f)
                    for (int c = 0; c < K; ++c)
                    {
                        const int l = spacefx::degreeOf (c), m = spacefx::orderOf (c);
                        if (m <= 0) continue;
                        const int cs = l * l + l - m;            // the sin partner
                        if (cs >= K) continue;
                        const float co = std::cos ((float) m * turn), si = std::sin ((float) m * turn);
                        r[(size_t) c]  = y[(size_t) c] * co - y[(size_t) cs] * si;
                        r[(size_t) cs] = y[(size_t) cs] * co + y[(size_t) c] * si;
                    }
                for (int c = 0; c < K; ++c) y[(size_t) c] = r[(size_t) c] * sgn[(size_t) c];
            }
            else if (swapLR && K > 1) std::swap (y[0], y[1]);
            // faded (low-pass), written back (with the copy itself when it is exposed again), added to the present
            for (int c = 0; c < K; ++c)
            {
                lp[(size_t) c] += lpA * (y[(size_t) c] - lp[(size_t) c]);
                const float old = lp[(size_t) c];
                float* b = buf.data() + (size_t) c * (size_t) len;
                const float now = ch[c][i];
                b[w & mask] = now + again * old;
                ch[c][i] = now + gSm * old;
            }
            ++w;
        }
    }

private:
    double sr = 48000.0;
    std::vector<float> buf;
    int len = 0;
    uint32_t mask = 0, w = 0;
    std::array<float, spacefx::kMaxCh> lp {};
    float dSm = -1.0f, gSm = 0.0f;
    bool wasOn = false;
};

class LongExposure
{
public:
    static constexpr int kDirs = 24;
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        fibonacciDirs (kDirs, az.data(), el.data(), 17.0f);
        for (int d = 0; d < kDirs; ++d)
        {
            float e[kMaxAmbiChannels] {};
            encodeAmbiX (az[(size_t) d], el[(size_t) d], 3, e);
            for (int c = 0; c < spacefx::kMaxCh; ++c)
            {
                enc[(size_t) d][(size_t) c] = e[c];
                dec[(size_t) d][(size_t) c] = e[c] * (float) (2 * spacefx::degreeOf (c) + 1) / (float) kDirs;
            }
            // every direction its own comb (30..71 ms) and two diffusing all-passes: trails without a common pitch
            const float u = (float) d / (float) kDirs;
            combLen[(size_t) d] = (int) ((0.030 + 0.041 * std::fmod (u * 7.31f + 0.13f, 1.0f)) * sr);
            ap1Len[(size_t) d] = (int) ((0.0047 + 0.0031 * std::fmod (u * 3.17f + 0.5f, 1.0f)) * sr);
            ap2Len[(size_t) d] = (int) ((0.0019 + 0.0017 * std::fmod (u * 5.71f + 0.2f, 1.0f)) * sr);
        }
        int total = 0;
        for (int d = 0; d < kDirs; ++d) total += combLen[(size_t) d] + ap1Len[(size_t) d] + ap2Len[(size_t) d] + 3;
        mem.assign ((size_t) total, 0.0f);
        int off = 0;
        for (int d = 0; d < kDirs; ++d)
        {
            combOff[(size_t) d] = off; off += combLen[(size_t) d] + 1;
            ap1Off[(size_t) d] = off; off += ap1Len[(size_t) d] + 1;
            ap2Off[(size_t) d] = off; off += ap2Len[(size_t) d] + 1;
        }
        reset();
    }
    void reset() noexcept
    {
        std::fill (mem.begin(), mem.end(), 0.0f);
        pos.fill (0); damp.fill (0.0f); gSm = 0.0f; wasOn = false;
    }

    void process (float* const* ch, int numCh, bool stereo, int n, const LongExposureParams& p) noexcept
    {
        if (mem.empty() || n <= 0) return;
        const bool on = p.on();
        if (! on && ! wasOn) return;
        if (on && ! wasOn) reset();
        const float gTarget = on ? p.mix : 0.0f;
        const int K = stereo ? 0 : juce::jmin (numCh, spacefx::kMaxCh);
        const int D = stereo ? 2 : kDirs;
        const float T = juce::jlimit (0.2f, 60.0f, p.time);
        const float dampA = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * juce::jmap (p.bright, 700.0f, 16000.0f) / (float) sr);
        std::array<float, kDirs> fb {}, norm {};
        for (int d = 0; d < D; ++d)
        {
            fb[(size_t) d] = std::pow (10.0f, -3.0f * (float) combLen[(size_t) d] / (T * (float) sr));
            norm[(size_t) d] = std::sqrt (1.0f - fb[(size_t) d] * fb[(size_t) d]) * (stereo ? 1.0f : 2.2f);   // steady level ~ the input's
        }
        float maxOut = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            gSm += 0.002f * (gTarget - gSm);
            std::array<float, spacefx::kMaxCh> outCh {};
            float outL = 0.0f, outR = 0.0f;
            for (int d = 0; d < D; ++d)
            {
                // what passes through this direction
                float x;
                if (stereo) x = ch[d][i];
                else
                {
                    x = 0.0f;
                    const auto& dc = dec[(size_t) d];
                    for (int c = 0; c < K; ++c) x += dc[(size_t) c] * ch[c][i];
                }
                // its own trail: two all-passes, a comb with a damped feedback
                float* cb = mem.data() + combOff[(size_t) d];
                float* a1 = mem.data() + ap1Off[(size_t) d];
                float* a2 = mem.data() + ap2Off[(size_t) d];
                const int p0 = pos[(size_t) d];
                const int cl = combLen[(size_t) d], l1 = ap1Len[(size_t) d], l2 = ap2Len[(size_t) d];
                const float yc = cb[p0 % cl];
                damp[(size_t) d] += dampA * (yc - damp[(size_t) d]);
                cb[p0 % cl] = norm[(size_t) d] * x + fb[(size_t) d] * damp[(size_t) d];
                float v = yc;
                { const float b = a1[p0 % l1]; const float t = v + 0.6f * b; a1[p0 % l1] = t; v = b - 0.6f * t; }
                { const float b = a2[p0 % l2]; const float t = v + 0.6f * b; a2[p0 % l2] = t; v = b - 0.6f * t; }
                pos[(size_t) d] = p0 + 1 >= cl * l1 * l2 ? 0 : p0 + 1;
                maxOut = juce::jmax (maxOut, std::abs (v));
                if (stereo) { if (d == 0) outL = v; else outR = v; }
                else { const auto& ec = enc[(size_t) d]; for (int c = 0; c < K; ++c) outCh[(size_t) c] += ec[(size_t) c] * v; }
            }
            if (stereo) { ch[0][i] += gSm * outL; if (numCh > 1) ch[1][i] += gSm * outR; }
            else for (int c = 0; c < K; ++c) ch[c][i] += gSm * outCh[(size_t) c];
        }
        wasOn = on || maxOut > 1.0e-6f;
    }

private:
    double sr = 48000.0;
    std::array<float, kDirs> az {}, el {};
    std::array<std::array<float, spacefx::kMaxCh>, kDirs> enc {}, dec {};
    std::array<int, kDirs> combLen {}, ap1Len {}, ap2Len {}, combOff {}, ap1Off {}, ap2Off {}, pos {};
    std::array<float, kDirs> damp {};
    std::vector<float> mem;
    float gSm = 0.0f;
    bool wasOn = false;
};

/** "Eis": a cascade of first-order all-passes per channel - low frequencies are held back more than high ones,
    so every reflection becomes a falling chirp from its own direction (like sound on frozen lakes) */
class DispersionBank
{
public:
    static constexpr int kMaxStages = 96;
    void reset() noexcept { for (auto& s : st) s.fill (0.0f); }
    /** x: channel buffers (in place); a = coefficient (< 0: the lows are delayed) */
    void process (float* const* x, int K, int n, int stages, float a) noexcept
    {
        K = juce::jmin (K, spacefx::kMaxCh);
        stages = juce::jlimit (0, kMaxStages, stages);
        for (int c = 0; c < K; ++c)
        {
            float* s = st[(size_t) c].data();
            float* d = x[c];
            for (int i = 0; i < n; ++i)
            {
                float v = d[i];
                for (int k = 0; k < stages; ++k)
                {
                    // y = a x + s; s = x - a y   (H = (a + z^-1) / (1 + a z^-1))
                    const float y = a * v + s[k];
                    s[k] = v - a * y;
                    v = y;
                }
                d[i] = v;
            }
        }
    }
private:
    std::array<std::array<float, kMaxStages>, spacefx::kMaxCh> st {};
};
} // namespace rysum
