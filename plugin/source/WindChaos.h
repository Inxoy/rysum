#pragma once

#include "Common.h"
#include <array>
#include <cmath>

namespace rysum
{
/** Wind model 2 ("instabil"): what a starving flue pipe does besides getting quiet, flat and dull.

    When the wind pressure at the foot drops, the jet leaving the flue gets slow. Besides the pipe
   tone a mouth tone becomes audible: the jet oscillating on the upper lip at its own frequency,
   which is set by the jet speed (~ square root of the pressure) and the mouth. It belongs to the
   pipe: it starts a few partials above the pipe's note and comes down towards it as the wind
   falls, so it
      - stays in place as long as the wind is steady,
      - glides downwards when the wind goes down, and gets quieter as it does,
      - leans on the pipe's own resonances (near the harmonics, a stopped pipe only the odd ones)
        and hops from one to the next as it passes them (some pipes more, some less),
      - is different from pipe to pipe (mouth, voicing), but always the same for the same pipe.
    It is not a clean whistle. A slow jet is turbulent: most of the mouth tone is breath - noise
   in a band around its frequency - with only a core of tone in it, and both flutter in pitch and
   loudness all the time, the more the weaker the wind.
    Every pipe gets its own mouth tone (where it starts, how far it reaches, how strongly it locks,
    a weaker second one on some pipes), derived from stop and key only, so it is reproducible.
    In addition: the breath of the pipe itself (noise coloured by the pipe, unsteady), and at very
    low wind the speech sputters (the fundamental drops out for a few milliseconds). Reeds: the
    tongue chatters instead of whistling.

    Everything here follows the stop wind (0 = closed .. 1 = fully drawn); at full wind nothing
   happens. */
struct WindChaos
{
    static constexpr int kTones = 2;

    struct EdgeTone
    {
        // per pipe (fixed)
        float fTop = 1200.0f; // Hz at the wind where it appears
        float wOn = 0.5f;     // stop wind below which it appears
        float amp = 0.0f;     // relative level (0 = this pipe has no such tone)
        float lock = 0.5f;    // 0 = glides freely, 1 = sits on the pipe resonances and hops
        float q = 18.0f;      // how narrow its breath is (a clearer or a more airy mouth)
        float tone = 0.35f;   // how much clean tone is in it
        // running
        int k = -1;                 // resonance it leans on (partial number)
        float f = 0.0f, a = 0.0f;   // smoothed frequency / level
        float re = 1.0f, im = 0.0f; // the core of tone (rotating phasor)
        float cr = 1.0f, ci = 0.0f; // rotation per sample
        // turbulence: a slow and a quick wander, for the pitch and for the loudness
        float slow = 0.0f, quick = 0.0f, gust = 0.0f, gain = 1.0f;
        // the same frequency as a noise band: the breath, the larger part of it
        float ic1 = 0.0f, ic2 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, nGain = 0.0f;
    };

    std::array<EdgeTone, kTones> tones{};
    XorShift rng{1};
    // the pipe's breath: noise in a wide band a few partials above the note, never quite even
    float hiss = 0.0f, hissGust = 0.0f, hissNow = 0.0f;
    float bc1 = 0.0f, bc2 = 0.0f, b1 = 0.0f, b2 = 0.0f, b3 = 0.0f, bGain = 0.0f;
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
        const float hz = juce::jmax(30.0f, pipeHz);
        for (int t = 0; t < kTones; ++t)
        {
            auto &e = tones[(size_t)t];
            e = EdgeTone{};
            // the mouth tone starts a few partials above the pipe's note (2.3 to 6 times it at
            // middle c; large pipes have relatively small mouths - theirs sits further up, small
            // pipes' nearer to the note)
            const float rel = std::pow(262.0f / hz, 0.35f);
            const float ratio =
                std::exp(std::log(2.3f) + (std::log(6.0f) - std::log(2.3f)) * pr.uni()) * rel;
            e.fTop = juce::jlimit(juce::jmax(180.0f, 1.3f * hz), fMax, hz * ratio);
            e.wOn = 0.3f + 0.25f * pr.uni();
            e.lock = pr.uni() * pr.uni() < 0.35f ? 0.3f + 0.6f * pr.uni() : 0.1f * pr.uni();
            e.amp = t == 0 ? 0.7f + 0.3f * pr.uni()
                           : (pr.uni() < 0.45f ? 0.25f + 0.25f * pr.uni() : 0.0f);
            e.q = 10.0f + 22.0f * pr.uni();
            e.tone = 0.2f + 0.3f * pr.uni();
            if (t == 1)
            {
                e.fTop *= 1.35f + 0.5f * pr.uni();
                e.fTop = juce::jmin(e.fTop, fMax);
                e.wOn *= 0.8f;
            }
        }
        hiss = hissGust = hissNow = 0.0f;
        bc1 = bc2 = 0.0f;
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
        // turbulence: noise smoothed over about a third of a second and over a few hundredths,
        // each brought to the same strength (unit variance: uniform noise has 1 / 3)
        const float kSlow = 1.0f - std::exp(-dtc / 0.30f), kQuick = 1.0f - std::exp(-dtc / 0.022f);
        const float nSlow = std::sqrt(3.0f * (2.0f - kSlow) / kSlow),
                    nQuick = std::sqrt(3.0f * (2.0f - kQuick) / kQuick);

        bool any = false;
        const float fMax = 0.45f * sr;
        for (auto &e : tones)
        {
            float target = 0.0f;
            float x = 1.0f;
            if (e.amp > 0.0f && w < e.wOn)
            {
                // appears below wOn (soft onset), then fades as the wind (and the jet) gets weaker
                x = w / e.wOn;
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
            // the jet is never still: the pitch wanders (slowly by a quarter of a semitone or
            // so, quickly by less), the loudness comes and goes - more the weaker the wind, less
            // when the tone sits on a resonance of the pipe
            e.slow += kSlow * (rng.bi() - e.slow);
            e.quick += kQuick * (rng.bi() - e.quick);
            e.gust += kQuick * (rng.bi() - e.gust);
            const float unrest = (0.6f + 0.9f * (1.0f - x)) * (1.0f - 0.5f * e.lock);
            const float cents = unrest * (16.0f * e.slow * nSlow + 7.0f * e.quick * nQuick);
            e.gain = juce::jlimit(0.15f, 2.2f,
                                  1.0f + unrest * (0.3f * e.gust * nQuick + 0.25f * e.slow * nSlow));
            const float hz = juce::jlimit(60.0f, fMax, e.f * std::exp2(cents / 1200.0f));
            const float wph = juce::MathConstants<float>::twoPi * hz / sr;
            e.cr = std::cos(wph);
            e.ci = std::sin(wph);
            const float n =
                1.0f / std::sqrt(e.re * e.re + e.im * e.im); // keep the phasor on the unit circle
            e.re *= n;
            e.im *= n;
            // the breath around it: a band of noise (peak gain 1), brought to about the level a
            // tone of amplitude 1 has
            const float q = e.q;
            const float g = std::tan(juce::MathConstants<float>::pi * hz / sr), kq = 1.0f / q;
            e.a1 = 1.0f / (1.0f + g * (g + kq));
            e.a2 = g * e.a1;
            e.a3 = g * e.a2;
            e.nGain = kq * std::sqrt(q * sr / (juce::MathConstants<float>::pi * hz)) * 1.0f;
        }

        // the pipe's own breath, strongest at low wind: noise in a wide band a few partials above
        // the note (what the pipe's body lets through), swelling and sinking a little
        const float hx = juce::jlimit(0.0f, 1.0f, (0.5f - w) / 0.4f);
        hiss += smooth * (0.18f * hx * hx * gate * (reed ? 0.5f : 1.0f) - hiss);
        hissGust += kSlow * (rng.bi() - hissGust);
        hissNow = hiss * juce::jlimit(0.3f, 1.8f, 1.0f + 0.35f * hissGust * nSlow);
        if (hiss > 1.0e-4f)
        {
            any = true;
            const float fc = juce::jlimit(700.0f, juce::jmin(5000.0f, 0.4f * sr),
                                          5.0f * juce::jmax(20.0f, f0));
            const float q = 0.9f, g = std::tan(juce::MathConstants<float>::pi * fc / sr),
                        kq = 1.0f / q;
            b1 = 1.0f / (1.0f + g * (g + kq));
            b2 = g * b1;
            b3 = g * b2;
            // (as loud as the hiss it replaces: white noise above 1.5 kHz)
            bGain = kq * std::sqrt(q * sr / (juce::MathConstants<float>::pi * fc)) * 0.75f;
        }

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
            // a core of tone (with a little of its octave: no mouth whistles a pure sine) in a
            // band of breath
            y += e.a * e.gain * (e.tone * (e.im + 0.5f * e.re * e.im) + v1 * e.nGain);
        }
        if (hissNow > 0.0f)
        {
            const float n2 = rng.bi();
            const float v3 = n2 - bc2;
            const float v1 = b1 * bc1 + b2 * v3;
            const float v2 = bc2 + b2 * bc1 + b3 * v3;
            bc1 = 2.0f * v1 - bc1;
            bc2 = 2.0f * v2 - bc2;
            y += v1 * bGain * hissNow;
        }
        return y;
    }
};
} // namespace rysum
