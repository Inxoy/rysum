#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <juce_audio_basics/juce_audio_basics.h>

namespace rysum
{
/** "Summe": the last stage of the output, for mixing and mastering - a three-band EQ (low shelf,
    a bell in the middle, high shelf) and a compressor, on whatever goes out (stereo, binaural,
    Ambisonics, loudspeakers). The EQ is the same filter on every channel (an Ambisonics field
    stays a field); the compressor hears the loudest channel and turns all of them down together
    (the image does not move). */
struct MasterSettings
{
    bool eqOn = false;
    float lowHz = 120.0f, lowDb = 0.0f;
    float midHz = 1000.0f, midDb = 0.0f, midQ = 0.7f;
    float highHz = 8000.0f, highDb = 0.0f;
    bool compOn = false;
    float threshDb = -12.0f, ratio = 2.5f, attackMs = 10.0f, releaseMs = 150.0f, makeupDb = 0.0f;
};

class MasterBus
{
public:
    static constexpr int kMaxCh = 64;

    struct Biquad
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    };

    // RBJ cookbook filters
    static Biquad shelf(double sr, float hz, float db, bool high) noexcept
    {
        const double A = std::pow(10.0, db / 40.0);
        const double w = juce::MathConstants<double>::twoPi * juce::jlimit(10.0, sr * 0.45, (double)hz) / sr;
        const double cw = std::cos(w), sw = std::sin(w);
        const double alpha = sw / 2.0 * std::sqrt(2.0); // shelf slope 1
        const double sA = 2.0 * std::sqrt(A) * alpha;
        double b0, b1, b2, a0, a1, a2;
        if (!high)
        {
            b0 = A * ((A + 1) - (A - 1) * cw + sA);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - sA);
            a0 = (A + 1) + (A - 1) * cw + sA;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - sA;
        }
        else
        {
            b0 = A * ((A + 1) + (A - 1) * cw + sA);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - sA);
            a0 = (A + 1) - (A - 1) * cw + sA;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - sA;
        }
        return {(float)(b0 / a0), (float)(b1 / a0), (float)(b2 / a0), (float)(a1 / a0), (float)(a2 / a0)};
    }
    static Biquad bell(double sr, float hz, float db, float q) noexcept
    {
        const double A = std::pow(10.0, db / 40.0);
        const double w = juce::MathConstants<double>::twoPi * juce::jlimit(10.0, sr * 0.45, (double)hz) / sr;
        const double alpha = std::sin(w) / (2.0 * juce::jmax(0.05, (double)q)), cw = std::cos(w);
        const double a0 = 1 + alpha / A;
        return {(float)((1 + alpha * A) / a0), (float)(-2 * cw / a0), (float)((1 - alpha * A) / a0),
                (float)(-2 * cw / a0), (float)((1 - alpha / A) / a0)};
    }
    /** the EQ's answer at hz in dB (for the curve on screen) */
    static float responseDb(const MasterSettings &s, double sr, float hz) noexcept
    {
        if (!s.eqOn)
            return 0.0f;
        const Biquad f[3]{shelf(sr, s.lowHz, s.lowDb, false), bell(sr, s.midHz, s.midDb, s.midQ),
                          shelf(sr, s.highHz, s.highDb, true)};
        const std::complex<double> z = std::polar(1.0, -juce::MathConstants<double>::twoPi * hz / sr);
        double mag = 1.0;
        for (const auto &b : f)
        {
            const auto num = (double)b.b0 + (double)b.b1 * z + (double)b.b2 * z * z;
            const auto den = 1.0 + (double)b.a1 * z + (double)b.a2 * z * z;
            mag *= std::abs(num / den);
        }
        return (float)(20.0 * std::log10(juce::jmax(1.0e-9, mag)));
    }

    void prepare(double sampleRate) noexcept
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        reset();
    }
    void reset() noexcept
    {
        for (auto &z : state)
            z = {0.0f, 0.0f};
        envDb = -120.0f;
        gainDb = 0.0f;
        eqWasOn = false;
    }

    void process(float *const *ch, int numCh, int n, const MasterSettings &s) noexcept
    {
        numCh = juce::jmin(numCh, kMaxCh);
        if (n <= 0 || numCh <= 0)
            return;
        // ---- EQ
        if (s.eqOn)
        {
            if (!eqWasOn)
                for (auto &z : state)
                    z = {0.0f, 0.0f};
            eqWasOn = true;
            const Biquad f[3]{shelf(sr, s.lowHz, s.lowDb, false), bell(sr, s.midHz, s.midDb, s.midQ),
                              shelf(sr, s.highHz, s.highDb, true)};
            const float dbs[3]{s.lowDb, s.midDb, s.highDb};
            for (int b = 0; b < 3; ++b)
            {
                if (std::abs(dbs[b]) < 0.01f)
                    continue; // (flat: nothing to do)
                const auto &q = f[b];
                for (int c = 0; c < numCh; ++c)
                {
                    float *x = ch[c];
                    auto &z = state[(size_t)(b * kMaxCh + c)];
                    float z1 = z[0], z2 = z[1];
                    for (int i = 0; i < n; ++i)
                    {
                        const float in = x[i];
                        const float out = q.b0 * in + z1;
                        z1 = q.b1 * in - q.a1 * out + z2;
                        z2 = q.b2 * in - q.a2 * out;
                        x[i] = out;
                    }
                    // (no denormals and no runaway after silence)
                    z[0] = std::abs(z1) < 1.0e-20f ? 0.0f : z1;
                    z[1] = std::abs(z2) < 1.0e-20f ? 0.0f : z2;
                }
            }
        }
        else
            eqWasOn = false;

        // ---- compressor: the loudest channel decides, every channel gets the same gain
        float peakIn = 0.0f, minGain = 0.0f;
        if (s.compOn)
        {
            const float ca = (float)std::exp(-1.0 / (juce::jmax(0.05, (double)s.attackMs) * 0.001 * sr));
            const float cr = (float)std::exp(-1.0 / (juce::jmax(1.0, (double)s.releaseMs) * 0.001 * sr));
            const float gs = (float)std::exp(-1.0 / (0.002 * sr)); // (the gain itself glides 2 ms)
            const float T = s.threshDb, R = juce::jmax(1.0f, s.ratio), W = 6.0f, slope = 1.0f / R - 1.0f;
            for (int i = 0; i < n; ++i)
            {
                float x = 0.0f;
                for (int c = 0; c < numCh; ++c)
                    x = juce::jmax(x, std::abs(ch[c][i]));
                peakIn = juce::jmax(peakIn, x);
                const float lev = 20.0f * std::log10(x + 1.0e-9f);
                envDb = lev + (lev > envDb ? ca : cr) * (envDb - lev);
                const float over = envDb - T;
                float gr = 0.0f;
                if (2.0f * over > W)
                    gr = slope * over;
                else if (2.0f * over > -W)
                {
                    const float k = over + W * 0.5f;
                    gr = slope * k * k / (2.0f * W);
                }
                gainDb = gr + gs * (gainDb - gr);
                minGain = juce::jmin(minGain, gainDb);
                const float g = std::pow(10.0f, (gainDb + s.makeupDb) / 20.0f);
                for (int c = 0; c < numCh; ++c)
                    ch[c][i] *= g;
            }
        }
        else
        {
            envDb = -120.0f;
            gainDb = 0.0f;
        }
        // the meter: the deepest reduction of the block, falling back slowly
        const float gr = -minGain;
        const float was = reductionDb.load(std::memory_order_relaxed);
        reductionDb.store(gr > was ? gr : was * 0.9f, std::memory_order_relaxed);
    }

    std::atomic<float> reductionDb{0.0f}; // how much the compressor turns down now (dB, >= 0)

private:
    double sr = 48000.0;
    std::array<std::array<float, 2>, 3 * kMaxCh> state{};
    float envDb = -120.0f, gainDb = 0.0f;
    bool eqWasOn = false;
};
} // namespace rysum
