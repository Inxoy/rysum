#pragma once

#include "Common.h"
#include <array>
#include <cmath>

namespace rysum
{
/** Wind model 2 ("instabil"): what a starving flue pipe does besides getting quiet, flat and dull.

    When the wind pressure at the foot drops, the jet leaving the flue gets slow and loses its grip
    on the air column. The pipe no longer locks firmly onto its fundamental:
      - the jet's own turbulence excites the pipe's upper resonances (they lie close to, but not
        exactly on, the harmonics; a stopped pipe only has the odd ones). Some of them catch for a
        moment and sound as thin whistles, jump to another resonance, glide, die away again;
      - the speech becomes intermittent: the fundamental drops out for a few milliseconds when the
        jet fails, and in those gaps the resonances take over;
      - there is more audible hiss from the mouth.
    Reeds behave differently: their tongue stops beating cleanly and chatters.

    Everything here is driven by one number, 'instability' (0 .. 1), which the engine derives from
    the stop wind: nothing at full wind, most around 10 % wind, nothing again when the stop is closed. */
struct WindChaos
{
    static constexpr int kRes = 3;

    struct Resonance
    {
        float f = 1000.0f, fTarget = 1000.0f, glide = 1.0f;     // Hz, control-rate glide coefficient
        int k = 0;                                              // which resonance (partial number)
        float env = 0.0f, envTarget = 0.0f, rise = 0.1f, fall = 0.1f;
        float q = 100.0f;
        int timer = 0;                                          // control ticks until the next decision
        float wob = 0.0f, wobTarget = 0.0f;                     // slow pitch wander (cents)
        // TPT state-variable band-pass
        float ic1 = 0.0f, ic2 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, gain = 0.0f;
    };

    std::array<Resonance, kRes> res {};
    XorShift rng { 1 };
    float level = 0.0f;           // smoothed instability (gated by the key)
    float jet = 1.0f, jetTarget = 1.0f;
    int jetTimer = 0;
    float hissLp = 0.0f, air = 0.0f;
    bool idle = true;

    void reset (uint32_t seed) noexcept
    {
        rng = XorShift (seed);
        rng.next();
        for (auto& r : res) r = Resonance {};
        for (auto& r : res) r.timer = (int) (rng.uni() * 40.0f);
        level = 0.0f; jet = jetTarget = 1.0f; jetTimer = 0; hissLp = 0.0f; air = 0.0f; idle = true;
    }

    /** how unstable a pipe is at this stop wind (0 = closed .. 1 = fully drawn) */
    static float instabilityForWind (float w) noexcept
    {
        const float x = juce::jlimit (0.0f, 1.0f, (0.55f - w) / 0.45f);
        return x * x * juce::jmin (1.0f, w / 0.06f);
    }

    /** multiplier for the pipe's own tone (the jet dropping out) */
    float toneGain() const noexcept { return 1.0f - 0.8f * level * (1.0f - jet); }

    /** control rate (every 'ticks' samples). f0 = frequency the pipe sounds at right now. */
    void control (float instability, float f0, bool stopped, bool reed, bool speaking, float sr, int ticks = 16) noexcept
    {
        const float dtc = (float) ticks / sr;
        const float target = speaking ? instability : 0.0f;
        level += (target > level ? 1.0f - std::exp (-dtc / 0.04f) : 1.0f - std::exp (-dtc / 0.06f)) * (target - level);

        bool anySounding = level > 1.0e-4f;
        for (auto& r : res) anySounding = anySounding || r.env > 1.0e-4f || r.envTarget > 0.0f;
        idle = ! anySounding;
        if (idle) { jet = jetTarget = 1.0f; return; }

        auto ms = [sr, ticks] (float m) { return juce::jmax (1, (int) (m * 0.001f * sr / (float) ticks)); };

        // ---- the jet: holds, fails for a moment, catches again
        if (--jetTimer <= 0)
        {
            const float pOff = (reed ? 0.75f : 0.5f) * std::pow (level, 1.2f);
            if (jetTarget > 0.5f && rng.uni() < pOff)
            {
                jetTarget = 0.0f;
                jetTimer = ms (reed ? 6.0f + 30.0f * rng.uni() : 12.0f + 110.0f * rng.uni() * rng.uni());
            }
            else
            {
                jetTarget = 1.0f;
                jetTimer = ms ((reed ? 15.0f + 60.0f * rng.uni() : 50.0f + 600.0f * rng.uni()) * (1.25f - 0.75f * level));
            }
        }
        jet += (1.0f - std::exp (-dtc / (reed ? 0.002f : 0.006f))) * (jetTarget - jet);

        // ---- resonances of the pipe body catching the jet noise
        const float fMax = juce::jmin (9500.0f, 0.42f * sr);
        const int kMin = stopped ? 3 : 2;
        const int kTop = (int) (fMax / juce::jmax (20.0f, f0));
        for (auto& r : res)
        {
            if (--r.timer <= 0)
            {
                const float pOn = level < 0.02f ? 0.0f : (reed ? 0.08f : 0.12f) + 0.38f * level;
                // mostly the lower resonances (2nd .. 8th), now and then a high squeak
                const int kMax = juce::jmin (kTop, rng.uni() < 0.15f ? 15 : (stopped ? 9 : 8));
                if (kMax >= kMin && rng.uni() < pOn)
                {
                    const float u = rng.uni();
                    int k = kMin + (int) ((float) (kMax - kMin + 1) * u * u);
                    if (stopped && (k % 2) == 0) k = k + 1 <= kMax ? k + 1 : k - 1;      // a stopped pipe only has the odd ones
                    k = juce::jlimit (kMin, juce::jmax (kMin, kMax), k);
                    // the resonances of a real pipe are a little sharp of the harmonics, and wander
                    const float fk = juce::jmin (fMax, f0 * (float) k * (1.0f + 0.0025f * (float) (k - 1)) * (1.0f + 0.006f * rng.bi()));
                    // a sounding resonance sometimes slides over to a neighbouring one, otherwise it jumps
                    const bool slide = r.env > 0.05f && std::abs (k - r.k) <= 2 && rng.uni() < 0.5f;
                    r.k = k;
                    r.fTarget = fk;
                    if (! slide) r.f = fk;
                    r.glide = 1.0f - std::exp (-dtc / (0.012f + 0.04f * rng.uni()));
                    r.q = 50.0f + 170.0f * rng.uni();
                    r.envTarget = 0.3f + 0.7f * rng.uni();
                    r.rise = 1.0f - std::exp (-dtc / (0.006f + 0.05f * rng.uni()));
                    r.fall = 1.0f - std::exp (-dtc / (0.03f + 0.15f * rng.uni()));
                    r.timer = ms ((40.0f + 400.0f * rng.uni() * rng.uni()) * (1.3f - 0.6f * level));
                }
                else
                {
                    r.envTarget = 0.0f;
                    r.timer = ms ((80.0f + 600.0f * rng.uni()) * (1.5f - 0.9f * level));
                }
                r.wobTarget = (10.0f + 30.0f * level) * rng.bi();
            }
            r.f += r.glide * (r.fTarget - r.f);
            r.wob += 0.05f * (r.wobTarget - r.wob);
            r.env += (r.envTarget > r.env ? r.rise : r.fall) * (r.envTarget - r.env);
            if (! speaking && r.envTarget > 0.0f) r.envTarget = 0.0f;   // the pallet closed: no more jet

            const float f = juce::jlimit (30.0f, 0.45f * sr, r.f * std::exp2 (r.wob / 1200.0f));
            const float g = std::tan (juce::MathConstants<float>::pi * f / sr);
            const float k = 1.0f / r.q;
            r.a1 = 1.0f / (1.0f + g * (g + k));
            r.a2 = g * r.a1;
            r.a3 = g * r.a2;
            // peak gain 1, then white noise in -> about the same RMS out; in the gaps of the jet they ring up
            const float boost = 1.0f + 1.5f * (1.0f - jet);
            r.gain = k * std::sqrt (r.q * sr / (juce::MathConstants<float>::pi * f)) * r.env * boost * (0.3f + 0.7f * level) * (reed ? 0.5f : 1.0f);
        }
    }

    /** one sample of the extra sound (to be added to the pipe, before the air filter) */
    inline float sample() noexcept
    {
        const float n = rng.bi();
        float y = 0.0f;
        for (auto& r : res)
        {
            const float v3 = n - r.ic2;
            const float v1 = r.a1 * r.ic1 + r.a2 * v3;
            const float v2 = r.ic2 + r.a2 * r.ic1 + r.a3 * v3;
            r.ic1 = 2.0f * v1 - r.ic1;
            r.ic2 = 2.0f * v2 - r.ic2;
            y += v1 * r.gain;
        }
        // hiss from the mouth: high-passed noise, louder when the jet fails
        hissLp += 0.35f * (n - hissLp);
        y += (n - hissLp) * 0.35f * level * (0.4f + 1.2f * (1.0f - jet));
        return y;
    }
};
} // namespace rysum
