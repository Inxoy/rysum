#pragma once

#include "Common.h"
#include <array>
#include <cmath>

namespace rysum
{
/** Wind model 2 ("instabil"): what a starving flue pipe does besides getting quiet, flat and dull.

    When the wind pressure at the foot drops, the jet leaving the flue gets slow. Besides the pipe
   tone an edge tone becomes audible: the jet oscillating on the upper lip at its own frequency,
   which is set by the jet speed (~ square root of the pressure) and the mouth. That makes it
      - steady as long as the wind is steady,
      - glide downwards when the wind goes down, and get quieter as it does,
      - lean on the pipe's own resonances (near the harmonics, a stopped pipe only the odd ones) and
        hop from one to the next as it passes them (some pipes more, some less),
      - different from pipe to pipe (mouth, voicing), but always the same for the same pipe.
    Every pipe gets its own edge tone (where it starts, how far it reaches, how strongly it locks, a
    weaker second one on some pipes), derived from stop and key only, so it is reproducible.
    In addition: a steady hiss from the mouth, and at very low wind the speech sputters (the
    fundamental drops out for a few milliseconds). Reeds: the tongue chatters instead of whistling.

    Everything here follows the stop wind (0 = closed .. 1 = fully drawn); at full wind nothing
   happens. */
struct WindChaos
{
    static constexpr int kTones = 2;

    struct EdgeTone
    {
        // per pipe (fixed)
        float fTop = 4000.0f; // Hz at the wind where it appears
        float wOn = 0.5f;     // stop wind below which it appears
        float amp = 0.0f;     // relative level (0 = this pipe has no such tone)
        float lock = 0.5f;    // 0 = glides freely, 1 = sits on the pipe resonances and hops
        // running
        int k = -1;                 // resonance it leans on (partial number)
        float f = 0.0f, a = 0.0f;   // smoothed frequency / level
        float re = 1.0f, im = 0.0f; // oscillator (rotating phasor)
        float cr = 1.0f, ci = 0.0f; // rotation per sample
        float jit = 0.0f;           // tiny wander (cents), like any real whistle
        // the same frequency as a narrow noise band: the breath around the whistle
        float ic1 = 0.0f, ic2 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, nGain = 0.0f;
    };

    std::array<EdgeTone, kTones> tones{};
    XorShift rng{1};
    float hiss = 0.0f, hissLp = 0.0f;
    float stutter = 0.0f; // 0 .. 1: how much the speech sputters
    float jet = 1.0f, jetTarget = 1.0f;
    int jetTimer = 0;
    bool idle = true;

    /** noiseSeed: per note; pipeSeed: per pipe (stop, key) - the pipe's character */
    void reset(uint32_t noiseSeed, uint32_t pipeSeed, float pipeHz, float sr) noexcept
    {
        rng = XorShift(noiseSeed);
        rng.next();
        XorShift pr(pipeSeed * 2654435761u + 0x5bd1e995u);
        for (int i = 0; i < 6; ++i)
            pr.next();
        const float fMax = juce::jmin(9000.0f, 0.4f * sr);
        for (int t = 0; t < kTones; ++t)
        {
            auto &e = tones[(size_t)t];
            e = EdgeTone{};
            // smaller pipes have smaller mouths: their edge tones sit higher
            const float size = std::pow(juce::jmax(30.0f, pipeHz) / 262.0f, 0.3f);
            e.fTop = juce::jlimit(
                900.0f, fMax,
                std::exp(std::log(2200.0f) + (std::log(7000.0f) - std::log(2200.0f)) * pr.uni()) *
                    size);
            e.wOn = 0.3f + 0.25f * pr.uni();
            e.lock = pr.uni() * pr.uni() < 0.35f ? 0.3f + 0.6f * pr.uni() : 0.1f * pr.uni();
            e.amp = t == 0 ? 0.7f + 0.3f * pr.uni()
                           : (pr.uni() < 0.45f ? 0.25f + 0.25f * pr.uni() : 0.0f);
            if (t == 1)
            {
                e.fTop *= 1.35f + 0.5f * pr.uni();
                e.fTop = juce::jmin(e.fTop, fMax);
                e.wOn *= 0.8f;
            }
        }
        hiss = hissLp = 0.0f;
        stutter = 0.0f;
        jet = jetTarget = 1.0f;
        jetTimer = 0;
        idle = true;
    }

    /** multiplier for the pipe's own tone (the speech sputtering at very low wind) */
    float toneGain() const noexcept
    {
        return 1.0f - 0.7f * stutter * (1.0f - jet);
    }

    /** control rate (every 'ticks' samples). w = stop wind (smoothed), f0 = frequency the pipe
     * sounds at right now. */
    void control(float w, float f0, bool stopped, bool reed, bool speaking, float sr,
                 int ticks = 16) noexcept
    {
        const float dtc = (float)ticks / sr;
        const float smooth = 1.0f - std::exp(-dtc / 0.03f);
        const float gate = speaking ? juce::jmin(1.0f, w / 0.04f) : 0.0f; // no wind, no sound

        bool any = false;
        const float fMax = 0.45f * sr;
        for (auto &e : tones)
        {
            float target = 0.0f;
            if (e.amp > 0.0f && w < e.wOn)
            {
                // appears below wOn (soft onset), then fades as the wind (and the jet) gets weaker
                const float x = w / e.wOn;
                const float onset = juce::jlimit(0.0f, 1.0f, (1.0f - x) / 0.12f);
                target = e.amp * onset * std::pow(x, 1.3f) * gate * (reed ? 0.5f : 1.0f);
            }
            e.a += smooth * (target - e.a);
            if (e.a < 1.0e-4f && target <= 0.0f)
            {
                e.a = 0.0f;
                e.k = -1;
                continue;
            }
            any = true;

            // free edge tone: proportional to the jet speed ~ sqrt (pressure)
            const float ff = e.fTop * std::sqrt(juce::jmax(0.005f, w) / e.wOn);
            // leaning on the nearest pipe resonance, with hysteresis: it only moves on when the
            // free tone has clearly left it, so at a steady wind it stays where it is
            const float fb = juce::jmax(20.0f, f0);
            auto resHz = [fb](int k) { return fb * (float)k * (1.0f + 0.0025f * (float)(k - 1)); };
            auto nearest = [&](float hz)
            {
                int k = juce::jmax(1, (int)std::lround(hz / fb));
                if (stopped && k % 2 == 0)
                    k += hz > resHz(k) ? 1 : -1;
                return juce::jmax(1, k);
            };
            if (e.k < 1)
                e.k = nearest(ff);
            else
            {
                const float spacing = stopped ? 2.0f : 1.0f;
                const float off = std::abs(ff - resHz(e.k)) / (fb * spacing);
                if (off > 0.5f + 0.3f * e.lock)
                    e.k = nearest(ff);
            }
            const float fLocked = resHz(e.k);
            const float fT = juce::jlimit(
                60.0f, fMax, std::exp(std::log(ff) + e.lock * (std::log(fLocked) - std::log(ff))));
            if (e.f <= 0.0f)
                e.f = fT;
            e.f += (1.0f - std::exp(-dtc / 0.025f)) * (fT - e.f);
            e.jit += 0.02f * (3.0f * rng.bi() - e.jit);
            const float hz = juce::jlimit(60.0f, fMax, e.f * std::exp2(e.jit / 1200.0f));
            const float wph = juce::MathConstants<float>::twoPi * hz / sr;
            e.cr = std::cos(wph);
            e.ci = std::sin(wph);
            const float n =
                1.0f / std::sqrt(e.re * e.re + e.im * e.im); // keep the phasor on the unit circle
            e.re *= n;
            e.im *= n;
            // breath band around it (narrow, peak gain 1)
            const float q = 60.0f;
            const float g = std::tan(juce::MathConstants<float>::pi * hz / sr), kq = 1.0f / q;
            e.a1 = 1.0f / (1.0f + g * (g + kq));
            e.a2 = g * e.a1;
            e.a3 = g * e.a2;
            e.nGain = kq * std::sqrt(q * sr / (juce::MathConstants<float>::pi * hz)) * 0.35f;
        }

        // steady hiss from the mouth, strongest at low wind
        const float hx = juce::jlimit(0.0f, 1.0f, (0.5f - w) / 0.4f);
        hiss += smooth * (0.18f * hx * hx * gate * (reed ? 0.5f : 1.0f) - hiss);
        any = any || hiss > 1.0e-4f;

        // very low wind: the jet sometimes fails for a few milliseconds
        const float st = juce::jlimit(0.0f, 1.0f, (0.16f - w) / 0.12f) * gate;
        stutter += smooth * (st - stutter);
        if (stutter > 0.01f)
        {
            any = true;
            if (--jetTimer <= 0)
            {
                auto ms = [sr, ticks](float m)
                { return juce::jmax(1, (int)(m * 0.001f * sr / (float)ticks)); };
                if (jetTarget > 0.5f && rng.uni() < (reed ? 0.6f : 0.35f) * stutter)
                {
                    jetTarget = 0.0f;
                    jetTimer = ms(reed ? 5.0f + 20.0f * rng.uni() : 8.0f + 50.0f * rng.uni());
                }
                else
                {
                    jetTarget = 1.0f;
                    jetTimer = ms(reed ? 15.0f + 50.0f * rng.uni() : 60.0f + 400.0f * rng.uni());
                }
            }
        }
        else
            jetTarget = 1.0f;
        jet += (1.0f - std::exp(-dtc / (reed ? 0.002f : 0.006f))) * (jetTarget - jet);
        idle = !any;
    }

    /** one sample of the extra sound (to be added to the pipe, before the air filter) */
    inline float sample() noexcept
    {
        const float n = rng.bi();
        float y = 0.0f;
        for (auto &e : tones)
        {
            if (e.a <= 0.0f)
                continue;
            const float re = e.re * e.cr - e.im * e.ci;
            e.im = e.re * e.ci + e.im * e.cr;
            e.re = re;
            const float v3 = n - e.ic2;
            const float v1 = e.a1 * e.ic1 + e.a2 * v3;
            const float v2 = e.ic2 + e.a2 * e.ic1 + e.a3 * v3;
            e.ic1 = 2.0f * v1 - e.ic1;
            e.ic2 = 2.0f * v2 - e.ic2;
            y += e.a * (e.im + v1 * e.nGain);
        }
        hissLp += 0.35f * (n - hissLp);
        y += (n - hissLp) * hiss;
        return y;
    }
};
} // namespace rysum
