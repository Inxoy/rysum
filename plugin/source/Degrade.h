#pragma once

#include "Common.h"

namespace rysum
{
/** "Verfall" – the degradation chain.
    amount   : overall wet amount (0 = untouched)
    grain    : signal-following noise + crackle (like film grain on the sound)
    bits     : bit depth of the quantiser (16 = off .. 3)
    rateHz   : sample-and-hold rate (48000 = off .. 600 Hz)
    wobble   : unstable wind – slow pitch and level fluctuation (applied in the voice)
    dropouts : random short gaps
*/
struct DegradeParams
{
    float amount = 0.0f, grain = 0.5f, bits = 12.0f, rateHz = 22050.0f, wobble = 0.2f,
          dropouts = 0.08f;
    bool perPipe = true;
    // derived values, set once per block by prepareBlock()
    float q = 0.0f, invQ = 0.0f; // quantiser steps (0 = off)
    double holdInc = 1.0;        // sample-and-hold phase increment
    float crackleProb = 0.0f, dropProb = 0.0f, dropStep = 0.0f;
    void prepareBlock(double sr) noexcept
    {
        q = bits < 15.9f ? std::pow(2.0f, bits - 1.0f) : 0.0f;
        invQ = q > 0.0f ? 1.0f / q : 0.0f;
        holdInc = juce::jlimit(1.0, sr, (double)rateHz) / sr;
        crackleProb = grain * grain * 40.0f / (float)sr;
        dropProb = dropouts * dropouts * 3.0f / (float)sr;
        dropStep = (float)(1.0 / (0.003 * sr));
    }
};

/** state of one degradation chain (one per voice in per-pipe mode, one for the sum otherwise).
    The random decisions are made per frame so that in sum mode all channels share them. */
struct DegradeState
{
    XorShift rng;
    double holdPhase = 1.0;
    float env = 0.0f;
    float dropGain = 1.0f;
    int dropRemain = 0;
    float crackle = 0.0f;

    explicit DegradeState(uint32_t seed = 7) : rng(seed) {}

    void reset(uint32_t seed)
    {
        rng = XorShift(seed);
        holdPhase = 1.0;
        env = 0.0f;
        dropGain = 1.0f;
        dropRemain = 0;
        crackle = 0.0f;
    }

    /** per-frame random decisions. Returns true when a new held sample must be taken. */
    struct Frame
    {
        bool take;
        float noise;
        float crackle;
        float drop;
    };

    inline Frame frame(const DegradeParams &p, double sr) noexcept
    {
        Frame f;
        // sample & hold
        holdPhase += p.holdInc;
        f.take = holdPhase >= 1.0;
        if (f.take)
            holdPhase -= 1.0;

        // grain: white noise + sparse crackle (decaying impulses)
        f.noise = rng.bi();
        if (rng.uni() < p.crackleProb)
            crackle = (rng.uni() < 0.5f ? -1.0f : 1.0f) * (0.5f + rng.uni());
        f.crackle = crackle;
        crackle = std::abs(crackle) > 1.0e-4f ? crackle * 0.93f : 0.0f;

        // dropouts: 20..250 ms gaps, 3 ms ramps
        if (dropRemain > 0)
            --dropRemain;
        else if (p.dropProb > 0.0f && rng.uni() < p.dropProb)
            dropRemain = (int)((0.02 + 0.23 * rng.uni()) * sr);
        const float target = dropRemain > 0 ? 0.0f : 1.0f;
        dropGain += juce::jlimit(-p.dropStep, p.dropStep, target - dropGain);
        f.drop = dropGain;
        return f;
    }

    /** degrade one sample. 'held' is this channel's sample-and-hold memory. */
    static inline float process(float x, float &held, float &envelope, const Frame &f,
                                const DegradeParams &p) noexcept
    {
        if (f.take)
            held = x;
        float y = held;
        if (p.q > 0.0f)
        {
            const float t = y * p.q;
            y = (float)(int)(t + (t >= 0.0f ? 0.5f : -0.5f)) * p.invQ;
        }
        envelope += 0.002f * (std::abs(x) - envelope);
        y += p.grain * envelope * (0.6f * f.noise + 1.5f * f.crackle);
        y *= f.drop;
        return x + p.amount * (y - x);
    }
};

/** slow random modulation for "Windschwankung": returns (pitchCents, gainFactor) per call */
struct Wobble
{
    double phase1 = 0, phase2 = 0, phase3 = 0;
    float f1 = 0.3f, f2 = 1.1f, f3 = 3.7f;
    void init(XorShift &r)
    {
        phase1 = r.uni();
        phase2 = r.uni();
        phase3 = r.uni();
        f1 = 0.15f + 0.4f * r.uni();
        f2 = 0.7f + 1.2f * r.uni();
        f3 = 2.5f + 3.0f * r.uni();
    }
    inline void advance(double seconds) noexcept
    {
        phase1 += f1 * seconds;
        phase2 += f2 * seconds;
        phase3 += f3 * seconds;
    }
    inline float value() const noexcept
    {
        const double tp = juce::MathConstants<double>::twoPi;
        return (float)((std::sin(tp * phase1) + 0.5 * std::sin(tp * phase2) +
                        0.15 * std::sin(tp * phase3)) /
                       1.65);
    }
};
} // namespace rysum
