#pragma once

#include "Common.h"
#include "Spatial.h"
#include <array>
#include <cmath>
#include <vector>

namespace rysum
{
/** "Raumverfall" – degradation of the spatial image (the counterpart of Körnung / Bittiefe / Abtastrate).
    Everything except the direction grid works on the Ambisonics channels, so it needs an Ambisonics
    output (AmbiX, binaural, loudspeakers); in stereo only the grid is heard.
    erode    : the order drops (0 = full order .. 1 = only W, omni), continuously between the orders
    weather  : seconds until the sphere has eroded completely while something sounds (0 = off);
               in silence it grows back four times as fast
    grid     : directions snap to a coarse grid (0 = off .. 1 = four directions)
    bits     : bit depth of the directional channels (16 = off); the higher the order, the coarser
    rateHz   : sample-and-hold rate of the directional channels (48000 = off); the higher the order, the lower
    drops    : spatial dropouts: single channels or whole orders fall silent for 50..400 ms
    alias    : the field is sampled at too few points and encoded again: spatial aliasing (ghost directions) */
struct SpaceDegParams
{
    float erode = 0.0f, weather = 0.0f, grid = 0.0f, bits = 16.0f, rateHz = 48000.0f, drops = 0.0f, alias = 0.0f;
    bool active() const noexcept
    {
        return erode > 1.0e-4f || weather > 0.0f || bits < 15.95f || rateHz < 47999.0f || drops > 1.0e-4f || alias > 1.0e-4f;
    }
};

/** "Körnerwolke" per stop: every pipe's sound is scattered as grains over the sphere around the pipe */
struct CloudParams
{
    float mix = 0.0f;          // 0 = off .. 1 = the pipe is dissolved into the cloud (equal power)
    float density = 30.0f;     // grains per second and pipe
    float spread = 60.0f;      // degrees around the pipe (180 = whole sphere)
    float sizeMs = 80.0f;      // grain length
    float scatterMs = 40.0f;   // how far back in the pipe's sound a grain may reach
    bool on() const noexcept { return mix > 1.0e-4f; }
};

/** number of grid directions for an amount 0..1 (0 = no grid) */
inline int gridCount (float grid) noexcept
{
    if (grid <= 1.0e-4f) return 0;
    return juce::jlimit (4, 64, juce::roundToInt (64.0f * std::pow (1.0f - juce::jlimit (0.0f, 1.0f, grid), 2.0f) + 4.0f * grid));
}

/** n directions spread evenly over the sphere (Fibonacci), as azimuth / elevation in degrees */
inline void fibonacciDirs (int n, float* az, float* el, float turnDeg = 0.0f) noexcept
{
    const double ga = juce::MathConstants<double>::pi * (3.0 - std::sqrt (5.0));
    for (int i = 0; i < n; ++i)
    {
        const double z = 1.0 - (2.0 * i + 1.0) / n;
        el[i] = (float) juce::radiansToDegrees (std::asin (z));
        az[i] = (float) std::remainder (juce::radiansToDegrees (ga * i) + turnDeg, 360.0);
    }
}

class SpaceEroder
{
public:
    void prepare (double sampleRate) { sr = sampleRate; reset(); }
    void reset() noexcept
    {
        held.fill (0.0f); holdPhase.fill (1.0); dropGain.fill (1.0f); dropRemain.fill (0); weight.fill (1.0f);
        age = 0.0; matrixOrder = -1; matrixK = -1; sinceRebuild = 0; turn = 0.0f; sub = 0;
    }

    /** the erosion heard right now (for the display), 0..1 */
    float erosionNow (const SpaceDegParams& p) const noexcept
    {
        return juce::jlimit (0.0f, 1.0f, p.erode + (p.weather > 0.0f ? (float) (age / p.weather) : 0.0f));
    }

    /** ch: the Ambisonics channels (AmbiX, ACN / SN3D) of 'order'; 'sounding': something plays (weathering) */
    void process (float* const* ch, int order, int n, const SpaceDegParams& p, bool sounding) noexcept
    {
        if (order <= 0 || n <= 0) return;
        const int C = juce::jmin (kMaxAmbiChannels, (order + 1) * (order + 1));
        // weathering: the sphere wears away while something sounds, and grows back in silence
        if (p.weather > 0.0f) age = juce::jlimit (0.0, (double) p.weather, age + (sounding ? 1.0 : -4.0) * n / sr);
        else age = 0.0;
        if (! p.active() && allQuiet()) return;

        // ---- spatial aliasing: decode to K points (too few for the order), encode again
        if (p.alias > 1.0e-4f)
        {
            const int full = (order + 1) * (order + 1);
            const int K = juce::jlimit (4, full, juce::roundToInt ((float) full * (1.0f - 0.85f * p.alias)));
            sinceRebuild += n;
            if (K != matrixK || order != matrixOrder || sinceRebuild >= 1024) { buildMatrix (order, K, sinceRebuild); sinceRebuild = 0; }
            const float wet = juce::jmin (1.0f, 2.0f * p.alias);
            float x[kMaxAmbiChannels];
            for (int i = 0; i < n; ++i)
            {
                for (int c = 0; c < C; ++c) x[c] = ch[c][i];
                for (int r = 0; r < C; ++r)
                {
                    const float* m = M.data() + (size_t) r * kMaxAmbiChannels;
                    float s = 0.0f;
                    for (int c = 0; c < C; ++c) s += m[c] * x[c];
                    ch[r][i] = x[r] + wet * (s - x[r]);
                }
            }
        }
        else matrixK = -1;

        // ---- erosion: the orders fade away from the top, continuously
        const float e = erosionNow (p);
        const float oEff = (float) order * (1.0f - e);
        // ---- bit depth / sample-and-hold per order (higher orders coarser), dropouts per channel
        const bool crush = p.bits < 15.95f, hold = p.rateHz < 47999.0f;
        float q[6] {}, inc[6] {};
        for (int l = 1; l <= order; ++l)
        {
            const float f = (float) l / (float) order;
            const float b = 16.0f - (16.0f - p.bits) * f;
            q[l] = crush ? std::pow (2.0f, b - 1.0f) : 0.0f;
            inc[l] = hold ? (float) (juce::jlimit (60.0, sr, 48000.0 * std::pow ((double) p.rateHz / 48000.0, (double) f)) / sr) : 1.0f;
        }
        const float dropStep = (float) (1.0 / (0.005 * sr));
        const double dropPerSec = (double) p.drops * p.drops * 3.0;
        for (int c = 1; c < C; ++c)
        {
            const int l = degreeOf (c);
            const float target = juce::jlimit (0.0f, 1.0f, oEff - (float) (l - 1));
            const float w0 = weight[(size_t) c], dw = (target - w0) / (float) n;
            float* x = ch[c];
            float& hd = held[(size_t) c];
            double& ph = holdPhase[(size_t) c];
            float& dg = dropGain[(size_t) c];
            int& dr = dropRemain[(size_t) c];
            for (int i = 0; i < n; ++i)
            {
                float y = x[i];
                if (hold) { ph += inc[l]; if (ph >= 1.0) { ph -= 1.0; hd = y; } y = hd; }
                if (q[l] > 0.0f) { const float t = y * q[l]; y = (float) (int) (t + (t >= 0.0f ? 0.5f : -0.5f)) / q[l]; }
                if (((sub + i) & 63) == 0 && dr <= 0 && dropPerSec > 0.0 && rng.uni() < (float) (dropPerSec * 64.0 / sr))
                {
                    const int len = (int) ((0.05 + 0.35 * rng.uni()) * sr);
                    if (rng.uni() < 0.3f)          // a whole order falls out
                        for (int c2 = l * l; c2 < (l + 1) * (l + 1) && c2 < C; ++c2) dropRemain[(size_t) c2] = juce::jmax (dropRemain[(size_t) c2], len);
                    else dr = len;
                }
                if (dr > 0) --dr;
                dg += juce::jlimit (-dropStep, dropStep, (dr > 0 ? 0.0f : 1.0f) - dg);
                x[i] = y * (w0 + dw * (float) (i + 1)) * dg;
            }
            weight[(size_t) c] = target;
        }
        sub += n;
    }

private:
    static int degreeOf (int acn) noexcept { return (int) std::floor (std::sqrt ((double) acn) + 1.0e-9); }
    bool allQuiet() const noexcept
    {
        for (int c = 1; c < kMaxAmbiChannels; ++c) if (weight[(size_t) c] < 1.0f || dropGain[(size_t) c] < 1.0f) return false;
        return true;
    }
    void buildMatrix (int order, int K, int elapsed) noexcept
    {
        // M = sum_k y_k (w o y_k)^T / K, w = 2l+1 (SN3D): with enough points M = identity, with too few: aliasing.
        // The sampling grid turns slowly, so the ghost directions wander.
        turn = std::fmod (turn + (float) (elapsed / sr) * 7.0f, 360.0f);
        float az[kMaxAmbiChannels], el[kMaxAmbiChannels];
        fibonacciDirs (K, az, el, turn);
        M.fill (0.0f);
        const int C = (order + 1) * (order + 1);
        float y[kMaxAmbiChannels];
        for (int k = 0; k < K; ++k)
        {
            std::fill (y, y + kMaxAmbiChannels, 0.0f);
            encodeAmbiX (az[k], el[k], order, y);
            for (int r = 0; r < C; ++r)
                for (int c = 0; c < C; ++c)
                    M[(size_t) r * kMaxAmbiChannels + (size_t) c] += y[r] * y[c] * (float) (2 * degreeOf (c) + 1) / (float) K;
        }
        matrixOrder = order; matrixK = K;
    }

    double sr = 48000.0;
    std::array<float, kMaxAmbiChannels> held {}, dropGain {}, weight {};
    std::array<double, kMaxAmbiChannels> holdPhase {};
    std::array<int, kMaxAmbiChannels> dropRemain {};
    std::array<float, kMaxAmbiChannels * kMaxAmbiChannels> M {};
    int matrixOrder = -1, matrixK = -1, sinceRebuild = 0;
    uint32_t sub = 0;
    float turn = 0.0f;
    double age = 0.0;
    XorShift rng { 0x5bace };
};
} // namespace rysum
