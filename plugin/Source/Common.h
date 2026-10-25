#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <algorithm>
#include <functional>
#include <cmath>

namespace rysum
{
constexpr int kNumStops = 7;
constexpr int kNumKeys  = 41;
constexpr int kMaxOrder = 5;
constexpr int kMaxAmbiChannels = (kMaxOrder + 1) * (kMaxOrder + 1);   // 36
constexpr int kMaxOutChannels = 64;
constexpr double kSampleFileRate = 48000.0;
constexpr double kA4 = 444.5;   // measured

// physical keys, left to right, as MIDI note of the 8' pitch they sound.
// short octave: the keys E F F# G G# A Bb B sound C F D G E A Bb B; no g#''.
constexpr std::array<int, kNumKeys> kPhysKeys {
    36, 41, 38, 43, 40, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
    60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
    72, 73, 74, 75, 76, 77, 78, 79,
    81 };

/** key index (0..40, physical order) for a MIDI note, -1 if the organ has no such pipe */
inline int keyIndexForMidi (int midi) noexcept
{
    for (int i = 0; i < kNumKeys; ++i)
        if (kPhysKeys[(size_t) i] == midi) return i;
    return -1;
}

/** key indices sorted by pitch (for layouts and displays) */
inline std::array<int, kNumKeys> keysByPitch() noexcept
{
    std::array<int, kNumKeys> k {};
    for (int i = 0; i < kNumKeys; ++i) k[(size_t) i] = i;
    std::sort (k.begin(), k.end(), [] (int a, int b) { return kPhysKeys[(size_t) a] < kPhysKeys[(size_t) b]; });
    return k;
}

constexpr double kMeantoneCents[12] { 0.0, 76.0, 193.2, 310.3, 386.3, 503.4, 579.5, 696.6, 772.6, 889.7, 1006.8, 1082.9 };

inline double meantoneHz (int midi) noexcept
{
    return kA4 * std::pow (2.0, (midi / 12 - 5) + (kMeantoneCents[midi % 12] - kMeantoneCents[9]) / 1200.0);
}

inline juce::String helmholtzName (int midi)
{
    static const char* names[12] { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B" };
    const int o = midi / 12 - 1;
    juce::String n (names[midi % 12]);
    if (o == 2) return n;
    n = n.toLowerCase();
    if (o == 3) return n;
    return n + juce::String::repeatedString (juce::CharPointer_UTF8 ("\xe2\x80\xb2"), o - 3);   // prime
}

struct StopInfo
{
    const char* id;
    const char* label;     // as on the organ
    const char* footage;
};

constexpr std::array<StopInfo, kNumStops> kStops {{
    { "praestant",    "Praestant 8'",  "8'" },
    { "gedackt",      "Gedackt 8'",    "8'" },
    { "oktave4",      "Octave 4'",     "4'" },
    { "oktave2",      "Octave 2'",     "2'" },
    { "sesquialtera", "Sesquialtera",  "II fach" },
    { "mixtur",       "Mixtur",        "III-IV fach" },
    { "trompete",     "Trompete 8'",   "8'" },
}};

inline int stopIndexForId (const juce::String& id)
{
    for (int i = 0; i < kNumStops; ++i)
        if (id == kStops[(size_t) i].id) return i;
    return -1;
}

inline juce::String prettyLabel (const char* s)
{
    return juce::String (s).replace ("'", juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xb2")));
}

/** small, fast, deterministic random generator usable on the audio thread */
struct XorShift
{
    uint32_t s = 0x9e3779b9u;
    explicit XorShift (uint32_t seed = 1) : s (seed ? seed : 1u) {}
    inline uint32_t next() noexcept { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    inline float uni() noexcept { return (float) (next() >> 8) * (1.0f / 16777216.0f); }          // 0..1
    inline float bi() noexcept  { return uni() * 2.0f - 1.0f; }                                   // -1..1
};

/** UTF-8 literal -> juce::String */
inline juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

inline float dbToGain (float db) noexcept { return db <= -99.0f ? 0.0f : std::pow (10.0f, db / 20.0f); }
} // namespace rysum
