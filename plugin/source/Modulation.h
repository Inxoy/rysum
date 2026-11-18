#pragma once

#include "Common.h"
#include "Lang.h"
#include <cmath>
#include <juce_core/juce_core.h>

namespace rysum
{
// ---------------------------------------------------------------------------
// Modulation: 4 LFOs, 2 envelopes, fixed sources, 12 routings
// ---------------------------------------------------------------------------
constexpr int kNumLfos = 4, kNumEnvs = 2, kNumRoutes = 24;

enum ModSource
{
    srcLfo1 = 0,
    srcLfo2,
    srcLfo3,
    srcLfo4,
    srcEnv1,
    srcEnv2,
    srcKey,        // key position, low = -1 .. high = +1
    srcModWheel,   // CC 1, 0..1
    srcRandom,     // random per note, -1..1
    srcVelocity,   // key velocity per note, 0..1 (global targets: the last note)
    srcPitchWheel, // pitch wheel, -1..1
    srcPressure,   // per note: MPE pressure / polyphonic aftertouch / channel pressure, 0..1
    srcSlide,      // per note: MPE "slide" (CC 74, the finger moving up and down the key), 0..1
    srcInput,      // loudness of the audio input, 0..1 (-54 .. 0 dB)
    srcBeat,    // per pipe: how fast it beats with the other sounding pipes, 0..1 (0 Hz .. 32 Hz,
                // logarithmic)
    srcBellows, // the wind pressure of the shared bellows ("Lebendige Orgel"), 0 = empty .. 1 =
                // full
    kNumSources
};

inline juce::StringArray modSourceNames()
{
    return {"LFO 1",
            "LFO 2",
            "LFO 3",
            "LFO 4",
            rysum::tr("H\xc3\xbc"
                                   "llkurve 1"),
            rysum::tr("H\xc3\xbc"
                                   "llkurve 2"),
            rysum::tr("Tastenlage"),
            rysum::tr("Modrad"),
            rysum::tr("Zufall je Ton"),
            rysum::tr("Anschlag"),
            rysum::tr("Tonh\xc3\xb6henrad"),
            rysum::tr("Druck (MPE)"),
            rysum::tr("Gleiten (MPE, CC 74)"),
            rysum::tr("Eingang (Pegel)"),
            rysum::tr("Schwebung"),
            rysum::tr("Balg (Winddruck)")};
}

/** targets 0..kNumPipeTargets-1 act on every pipe separately: only here do the per-pipe sources
   differ from pipe to pipe (LFO "Versatz", envelopes "pro Pfeife", key position, random per note,
   velocity, MPE pressure / slide). targets >= kGlobalTargetBase are plugin parameters (index into
   the processor's parameter list): one value for the whole instrument - an envelope "pro Pfeife"
   acts there like a global one (triggered by every key), the LFO's "Versatz" does nothing, key
   position / velocity are those of the last note. */
enum PipeTarget
{
    tgtAz = 0, // direction, +-180 deg at full amount
    tgtEl,     // height, +-90 deg
    tgtDist,   // distance, x8 / :8
    tgtLevel,  // level, +-36 dB (max +12 dB)
    tgtPitch,  // pitch, +-12 semitones
    tgtBright, // brightness (low-pass), +-6 octaves
    tgtSend,   // room send, +-24 dB
    // (new targets only at the end: projects and patches store "pipe:<index>")
    tgtWind,    // the pipe's own stop wind ("Registerwind"), +-100 % (0 % = the pipe falls silent)
    tgtDegrade, // the pipe's amount of "Verfall" (per-pipe mode), +-100 %
    tgtWobble,  // the pipe's "Windschwankung", +-100 %
    tgtCrackle, // the pipe's crackle ("Knistern", Effekte page), +-100 %
    // the pipe's own sound (Raum page, tab "Pfeife"), on top of its stop's setting
    tgtAttack,    // Ansprache, +-1 (-1 soft swell .. +1 chiff)
    tgtAttSharp,  // Ansprache Schaerfe, +-100 %
    tgtAttIso,    // Nur Ansprache, +-100 %
    tgtAttRep,    // Ansprache wiederholen, +-100 % (taken when the note starts)
    tgtPuff,      // Abriss, +-100 % (taken when the key is released)
    tgtHiFreq,    // Obertoene ab, +-4 octaves
    tgtHiGain,    // Obertoene Pegel, +-24 dB
    tgtHiShape,   // Obertoene Anschlag, +-100 %
    tgtAirInvert, // Luft umkehren, +-100 %
    // the pipe's grain cloud (Raum page, tab "Wolke")
    tgtCloudMix,     // Menge, +-100 %
    tgtCloudDens,    // Dichte, x8 / :8
    tgtCloudSpread,  // Streuung, +-180 deg
    tgtCloudSize,    // Korngroesse, x8 / :8
    tgtCloudScatter, // Zeitstreuung, +-150 ms
    // the pipe's Verfall (only in the mode "pro Pfeife")
    tgtDegGrain, // Koernung, +-100 %
    tgtDegBits,  // Bittiefe, +-13 bits
    tgtDegRate,  // Abtastrate, +-6 octaves
    tgtDegDrops, // Aussetzer, +-100 %
    // the pipe's glitches (Spieltisch, Verfall - Glitch)
    tgtStutter,
    tgtSplice,
    tgtZero,
    tgtClip, // +-100 % (harte Kanten: at the start and the release of the note)
    kNumPipeTargets
};
constexpr int kGlobalTargetBase = 100;

inline juce::StringArray pipeTargetNames()
{
    return {"Pfeife: Richtung",
            rysum::tr("Pfeife: H\xc3\xb6he"),
            rysum::tr("Pfeife: Abstand"),
            rysum::tr("Pfeife: Lautst\xc3\xa4rke"),
            rysum::tr("Pfeife: Tonh\xc3\xb6he"),
            rysum::tr("Pfeife: Helligkeit"),
            rysum::tr("Pfeife: Hall-Anteil"),
            rysum::tr("Pfeife: Wind"),
            rysum::tr("Pfeife: Verfall"),
            rysum::tr("Pfeife: Windschwankung"),
            rysum::tr("Pfeife: Knistern"),
            rysum::tr("Pfeife: Ansprache"),
            rysum::tr("Pfeife: Ansprache Sch\xc3\xa4rfe"),
            rysum::tr("Pfeife: Nur Ansprache"),
            rysum::tr("Pfeife: Ansprache wiederholen"),
            rysum::tr("Pfeife: Abriss"),
            rysum::tr("Pfeife: Obert\xc3\xb6ne ab"),
            rysum::tr("Pfeife: Obert\xc3\xb6ne Pegel"),
            rysum::tr("Pfeife: Obert\xc3\xb6ne Anschlag"),
            rysum::tr("Pfeife: Luft umkehren"),
            rysum::tr("Pfeife: Wolke Menge"),
            rysum::tr("Pfeife: Wolke Dichte"),
            rysum::tr("Pfeife: Wolke Streuung"),
            rysum::tr("Pfeife: Wolke Korngr\xc3\xb6\xc3\x9f"
                                   "e"),
            rysum::tr("Pfeife: Wolke Zeitstreuung"),
            rysum::tr("Pfeife: K\xc3\xb6rnung"),
            rysum::tr("Pfeife: Bittiefe"),
            rysum::tr("Pfeife: Abtastrate"),
            rysum::tr("Pfeife: Aussetzer"),
            rysum::tr("Pfeife: Glitch Stottern"),
            rysum::tr("Pfeife: Glitch Bandschnitt"),
            rysum::tr("Pfeife: Glitch harte Kanten"),
            rysum::tr("Pfeife: Glitch Clipping")};
}
inline juce::String pipeTargetId(int t)
{
    return "pipe:" + juce::String(t);
}

enum LfoShape
{
    shSine = 0,
    shTriangle,
    shSaw,
    shSquare,
    shSteps,
    shSmooth,
    kNumShapes
};
inline juce::StringArray lfoShapeNames()
{
    return {"Sinus",    "Dreieck",         juce::String::fromUTF8("S\xc3\xa4gezahn"),
            "Rechteck", "Zufall (Stufen)", "Zufall (weich)"};
}

/** tempo divisions: quarter notes per cycle */
inline juce::StringArray lfoDivNames()
{
    return {"4 Takte", "2 Takte", "1 Takt", "1/2",   "1/4",   "1/8",
            "1/16",    "1/32",    "1/4 T",  "1/8 T", "1/16 T"};
}
inline double lfoDivQuarters(int d)
{
    static const double q[]{16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0};
    return q[juce::jlimit(0, 10, d)];
}

struct LfoParams
{
    int shape = shSine;
    float rate = 0.5f; // Hz (free)
    bool sync = false;
    int div = 2;
    float spread = 0.0f; // 0 = all pipes together .. 1 = a full cycle across the pipes
    int spreadMode = 0;  // 0 = along the keyboard, 1 = random per pipe
};

struct EnvParams
{
    bool perPipe = true; // every pipe its own envelope / one global envelope
    float delay = 0.0f, attack = 0.3f, decay = 1.0f, sustain = 0.6f,
          release = 1.5f; // seconds / level
};

struct ModRoute
{
    int src = -1, target = -1;
    float amount = 0.0f;
    bool uni = false;
};

/** LFOs, key position and the per-note random swing -1..1; envelopes and the mod wheel are 0..1 */
inline bool isBipolarSource(int s) noexcept
{
    return (s >= srcLfo1 && s < srcLfo1 + kNumLfos) || s == srcKey || s == srcRandom ||
           s == srcPitchWheel;
}

/** unipolar routing: a bipolar source is mapped to 0..1 (only pushes the target one way) */
inline float shapeSource(int s, float v, bool uni) noexcept
{
    return uni && isBipolarSource(s) ? 0.5f * (v + 1.0f) : v;
}

/** range of a routing's source after shaping (for drawing the modulation range) */
inline float sourceMin(int s, bool uni) noexcept
{
    return isBipolarSource(s) && !uni ? -1.0f : 0.0f;
}

struct ModParams
{
    std::array<LfoParams, kNumLfos> lfo{};
    std::array<EnvParams, kNumEnvs> env{};
    std::array<ModRoute, kNumRoutes> routes{};
};

// ---------------------------------------------------------------------------
inline float hash01(uint32_t a) noexcept
{
    a ^= a >> 16;
    a *= 0x7feb352du;
    a ^= a >> 15;
    a *= 0x846ca68bu;
    a ^= a >> 16;
    return (float)(a & 0xffffff) / (float)0xffffff;
}

/** LFO value -1..1 at a phase in cycles; seed varies the random shapes */
inline float lfoValue(int shape, double phase, uint32_t seed) noexcept
{
    const double fl = std::floor(phase);
    const float f = (float)(phase - fl);
    switch (shape)
    {
    // shapes are phase-aligned with the sine: start at 0, rise first
    case shTriangle:
        return f < 0.25f ? 4.0f * f : (f < 0.75f ? 2.0f - 4.0f * f : 4.0f * f - 4.0f);
    case shSaw:
        return f < 0.5f ? 2.0f * f : 2.0f * f - 2.0f;
    case shSquare:
        return f < 0.5f ? 1.0f : -1.0f;
    case shSteps:
        return 2.0f * hash01((uint32_t)(int64_t)fl * 2654435761u ^ (seed * 40503u + 17u)) - 1.0f;
    case shSmooth:
    {
        const float a =
            2.0f * hash01((uint32_t)(int64_t)fl * 2654435761u ^ (seed * 40503u + 17u)) - 1.0f;
        const float b =
            2.0f * hash01((uint32_t)(int64_t)(fl + 1.0) * 2654435761u ^ (seed * 40503u + 17u)) -
            1.0f;
        const float s = f * f * (3.0f - 2.0f * f);
        return a + (b - a) * s;
    }
    case shSine:
    default:
        return (float)std::sin(juce::MathConstants<double>::twoPi * phase);
    }
}

/** delay-attack-decay-sustain-release, advanced in steps (every 64 samples) */
struct EnvState
{
    enum Stage
    {
        Idle,
        Delay,
        Attack,
        Decay,
        Sustain,
        Release
    };
    Stage stage = Idle;
    float level = 0.0f, t = 0.0f;
    bool gate = false;

    void trigger() noexcept
    {
        stage = Delay;
        t = 0.0f;
        gate = true;
    } // retriggers from the current level
    void releaseGate() noexcept
    {
        gate = false;
        if (stage != Idle)
            stage = Release;
    }
    void reset() noexcept
    {
        stage = Idle;
        level = 0.0f;
        t = 0.0f;
        gate = false;
    }

    void advance(const EnvParams &e, float dt) noexcept
    {
        switch (stage)
        {
        case Idle:
            level = 0.0f;
            break;
        case Delay:
            t += dt;
            if (t >= e.delay)
            {
                stage = Attack;
                t = 0.0f;
            }
            break;
        case Attack:
            level += dt / juce::jmax(0.001f, e.attack);
            if (level >= 1.0f)
            {
                level = 1.0f;
                stage = Decay;
            }
            break;
        case Decay:
            level = e.sustain +
                    (level - e.sustain) * std::exp(-dt * 4.0f / juce::jmax(0.001f, e.decay));
            if (std::abs(level - e.sustain) < 0.001f)
            {
                level = e.sustain;
                stage = Sustain;
            }
            break;
        case Sustain:
            level = e.sustain;
            break;
        case Release:
            level *= std::exp(-dt * 4.0f / juce::jmax(0.001f, e.release));
            if (level < 0.0005f)
            {
                level = 0.0f;
                stage = Idle;
            }
            break;
        }
    }
};
} // namespace rysum
