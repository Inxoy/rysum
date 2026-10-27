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
    // new entries only at the end: projects and patches store the index
    return { juce::String::fromUTF8 ("\xc2\xbc-Komma mittelt\xc3\xb6nig (Original)"), juce::String::fromUTF8 ("\xe2\x85\x99-Komma mittelt\xc3\xb6nig"),
             "Werckmeister III", "Kirnberger III", "Vallotti", "Pythagoreisch", "Gleichstufig",
             "Rein (5-Limit, C-Dur)", "Rein septimal (7-Limit)", juce::String::fromUTF8 ("Naturtonreihe auf C (16.\xe2\x80\x93" "30. Teilton)"),
             "31-EDO (12 aus 31)", "19-EDO (12 aus 19)", "53-EDO (12 aus 53, fast rein)",
             juce::String::fromUTF8 ("Maqam Rast (E und H \xc2\xbc-Ton tiefer)"),
             juce::String::fromUTF8 ("31-EDO \xc2\xb7 jede Taste ein Schritt (38,7 ct)"),
             juce::String::fromUTF8 ("19-EDO \xc2\xb7 jede Taste ein Schritt (63,2 ct)"),
             juce::String::fromUTF8 ("24-EDO \xc2\xb7 Viertelt\xc3\xb6ne (50 ct)"),
             juce::String::fromUTF8 ("Bohlen-Pierce \xc2\xb7 13 Stufen pro Duodezime (146,3 ct)"),
             juce::String::fromUTF8 ("Carlos Alpha \xc2\xb7 jede Taste 78 ct") };
}

/** tunings from index 14 on: every MIDI note is one step of this size (cents), a' stays put */
inline double tuningStepCents (int t) noexcept
{
    switch (t)
    {
        case 14: return 1200.0 / 31.0;
        case 15: return 1200.0 / 19.0;
        case 16: return 50.0;
        case 17: return 1200.0 * std::log2 (3.0) / 13.0;
        case 18: return 77.965;
        default: return 0.0;
    }
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
        case 7: case 8: case 9:
        {
            // just intonation on C: ratios per pitch class
            static const double r5[12] { 1.0, 16.0 / 15, 9.0 / 8, 6.0 / 5, 5.0 / 4, 4.0 / 3, 45.0 / 32, 3.0 / 2, 8.0 / 5, 5.0 / 3, 9.0 / 5, 15.0 / 8 };
            static const double r7[12] { 1.0, 15.0 / 14, 8.0 / 7, 6.0 / 5, 5.0 / 4, 4.0 / 3, 7.0 / 5, 3.0 / 2, 8.0 / 5, 5.0 / 3, 7.0 / 4, 15.0 / 8 };
            static const double rh[12] { 16.0 / 16, 17.0 / 16, 18.0 / 16, 19.0 / 16, 20.0 / 16, 21.0 / 16, 22.0 / 16, 24.0 / 16, 26.0 / 16, 27.0 / 16, 28.0 / 16, 30.0 / 16 };
            const double* r = t == 7 ? r5 : (t == 8 ? r7 : rh);
            std::array<double, 12> c {};
            for (int i = 0; i < 12; ++i) c[(size_t) i] = 1200.0 * std::log2 (r[i]);
            return c;
        }
        case 10: case 11: case 12:
        {
            // 12 of n equal steps (the chromatic notes where meantone / just intonation puts them)
            static const int s31[12] { 0, 2, 5, 8, 10, 13, 15, 18, 20, 23, 26, 28 };
            static const int s19[12] { 0, 1, 3, 5, 6, 8, 9, 11, 12, 14, 16, 17 };
            static const int s53[12] { 0, 5, 9, 14, 17, 22, 26, 31, 36, 39, 45, 48 };
            const int n = t == 10 ? 31 : (t == 11 ? 19 : 53);
            const int* s = t == 10 ? s31 : (t == 11 ? s19 : s53);
            std::array<double, 12> c {};
            for (int i = 0; i < 12; ++i) c[(size_t) i] = 1200.0 * s[i] / n;
            return c;
        }
        case 13: return { 0, 100, 200, 300, 350, 500, 600, 700, 800, 900, 1000, 1050 };
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

/** Tuning of every key: shift in cents from the recorded pipe that is played (source[k]) to the
    pitch key k should have. Twelve-note tunings keep every key's own pipe; the step tunings
    (every MIDI note one step) take the recorded pipe nearest to the wanted pitch, so no pipe is
    shifted far (except outside the organ's range). */
struct KeyTuning
{
    std::array<float, kNumKeys> cents {};
    std::array<int, kNumKeys> source {};
};

inline KeyTuning makeKeyTuning (int t)
{
    KeyTuning kt;
    auto recorded = [] (int midi) { return 1200.0 * std::log2 (meantoneHz (midi) / meantoneHz (69)); };   // cents from a'
    const double step = tuningStepCents (t);
    if (step <= 0.0)
    {
        const auto sh = temperamentShift (t);
        for (int k = 0; k < kNumKeys; ++k) { kt.source[(size_t) k] = k; kt.cents[(size_t) k] = sh[(size_t) (kPhysKeys[(size_t) k] % 12)]; }
        return kt;
    }
    for (int k = 0; k < kNumKeys; ++k)
    {
        const double target = (kPhysKeys[(size_t) k] - 69) * step;
        int best = k; double bd = 1e9;
        for (int s = 0; s < kNumKeys; ++s)
        {
            const double d = std::abs (recorded (kPhysKeys[(size_t) s]) - target);
            if (d < bd) { bd = d; best = s; }
        }
        kt.source[(size_t) k] = best;
        kt.cents[(size_t) k] = (float) (target - recorded (kPhysKeys[(size_t) best]));
    }
    return kt;
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
