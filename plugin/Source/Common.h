#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <algorithm>
#include <functional>
#include <cmath>

namespace rysum
{
constexpr int kNumStops = 8;
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

// --------------------------------------------------------------------------
// Temperaments (cents above C). The samples are in 1/4-comma meantone; another
// temperament is reached by shifting each pitch class, a' stays where it is.
// --------------------------------------------------------------------------
inline juce::StringArray temperamentNames()
{
    return { juce::String::fromUTF8 ("\xc2\xbc-Komma mittelt\xc3\xb6nig (Original)"), juce::String::fromUTF8 ("\xe2\x85\x99-Komma mittelt\xc3\xb6nig"),
             "Werckmeister III", "Kirnberger III", "Vallotti", "Pythagoreisch", "Gleichstufig" };
}

inline std::array<double, 12> temperamentCents (int t)
{
    switch (t)
    {
        case 1:
        {
            const double f = 701.955 - 21.506 / 6.0;           // 1/6-comma fifth
            auto m = [] (double v) { return std::fmod (std::fmod (v, 1200.0) + 1200.0, 1200.0); };
            return { 0.0, m (7 * f), m (2 * f), m (-3 * f), m (4 * f), m (-1 * f), m (6 * f), m (f), m (8 * f), m (3 * f), m (-2 * f), m (5 * f) };
        }
        case 2: return { 0, 90.225, 192.180, 294.135, 390.225, 498.045, 588.270, 696.090, 792.180, 888.270, 996.090, 1092.180 };
        case 3: return { 0, 90.225, 193.157, 294.135, 386.314, 498.045, 590.224, 696.578, 792.180, 889.735, 996.090, 1088.269 };
        case 4: return { 0, 94.135, 196.090, 298.045, 392.180, 501.955, 592.180, 698.045, 796.090, 894.135, 1000.000, 1090.225 };
        case 5: return { 0, 113.685, 203.910, 294.135, 407.820, 498.045, 611.730, 701.955, 815.640, 905.865, 996.090, 1109.775 };
        case 6: return { 0, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100 };
        default: return { kMeantoneCents[0], kMeantoneCents[1], kMeantoneCents[2], kMeantoneCents[3], kMeantoneCents[4], kMeantoneCents[5],
                          kMeantoneCents[6], kMeantoneCents[7], kMeantoneCents[8], kMeantoneCents[9], kMeantoneCents[10], kMeantoneCents[11] };
    }
}

/** shift in cents per pitch class to go from the recorded meantone to temperament t (a' fixed) */
inline std::array<float, 12> temperamentShift (int t)
{
    const auto c = temperamentCents (t);
    std::array<float, 12> s {};
    for (int i = 0; i < 12; ++i)
        s[(size_t) i] = (float) ((c[(size_t) i] - c[9]) - (kMeantoneCents[i] - kMeantoneCents[9]));
    return s;
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
    { "praestant16",  "Praestant 16'", "16'" },     // made from the Praestant 8' an octave lower
}};

/** order on screen: the 16' first, as on a stop list (parameter / state indices stay as they are) */
constexpr std::array<int, kNumStops> kDisplayOrder { 7, 0, 1, 2, 3, 4, 5, 6 };
constexpr int kPraestant16 = 7;

/** default stop volumes (dB): measured perceived loudness (K-weighted, all pipes) of the cleaned samples,
    including the default distances in the room, so all stops start roughly equally loud */
constexpr std::array<float, kNumStops> kDefaultStopVolume { -2.0f, 4.0f, -0.5f, 1.0f, 1.0f, -0.5f, -1.5f, 0.5f };
inline int displaySlot (int stop) { for (int i = 0; i < kNumStops; ++i) if (kDisplayOrder[(size_t) i] == stop) return i; return stop; }

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
