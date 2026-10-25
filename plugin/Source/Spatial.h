#pragma once

#include "Common.h"

namespace rysum
{
/** Position on the sphere in degrees. Azimuth: 0 = front, +90 = left (AmbiX convention).
    Elevation: 0 = horizon, +90 = zenith. */
struct Pos { float az = 0.0f, el = 0.0f; };

inline float wrapAz (float a) noexcept
{
    a = std::fmod (a + 180.0f, 360.0f);
    if (a < 0.0f) a += 360.0f;
    return a - 180.0f;
}

// --------------------------------------------------------------------------
// Encoder: real spherical harmonics, ACN channel order, SN3D normalisation,
// no Condon-Shortley phase  (= AmbiX, as used by the IEM suite / Reaper).
// --------------------------------------------------------------------------
inline void encodeAmbiX (float azDeg, float elDeg, int order, float* out) noexcept
{
    const double az = azDeg * juce::MathConstants<double>::pi / 180.0;
    const double el = elDeg * juce::MathConstants<double>::pi / 180.0;
    const double x  = std::sin (el);                 // argument of the Legendre functions
    const double c  = std::cos (el);                 // sqrt(1 - x^2)

    // associated Legendre P[l][m] without CS phase
    double P[kMaxOrder + 1][kMaxOrder + 1] {};
    P[0][0] = 1.0;
    for (int m = 1; m <= order; ++m)
        P[m][m] = (2 * m - 1) * c * P[m - 1][m - 1];
    for (int m = 0; m < order; ++m)
        P[m + 1][m] = x * (2 * m + 1) * P[m][m];
    for (int m = 0; m <= order; ++m)
        for (int l = m + 2; l <= order; ++l)
            P[l][m] = ((2 * l - 1) * x * P[l - 1][m] - (l + m - 1) * P[l - 2][m]) / (double) (l - m);

    static const double fact[] { 1, 1, 2, 6, 24, 120, 720, 5040, 40320, 362880, 3628800 };

    for (int l = 0; l <= order; ++l)
    {
        for (int m = -l; m <= l; ++m)
        {
            const int am = std::abs (m);
            const double n = std::sqrt ((am == 0 ? 1.0 : 2.0) * fact[l - am] / fact[l + am]);
            const double trig = m > 0 ? std::cos (am * az) : (m < 0 ? std::sin (am * az) : 1.0);
            out[l * l + l + m] = (float) (n * P[l][am] * trig);
        }
    }
}

/** equal-power stereo panning from azimuth (front and back fold onto the same pan) */
inline void encodeStereo (float azDeg, float* out) noexcept
{
    const float p = std::sin (azDeg * juce::MathConstants<float>::pi / 180.0f);   // +1 = left
    const float ang = (1.0f - p) * juce::MathConstants<float>::pi * 0.25f;          // 0 = left, pi/2 = right
    out[0] = std::cos (ang);
    out[1] = std::sin (ang);
}

// --------------------------------------------------------------------------
// Motion
// --------------------------------------------------------------------------
enum class Motion { Static = 0, Rotation, Pendulum, Drift };

inline juce::StringArray motionNames() { return { "Statisch", "Rotation", "Pendel", "Drift" }; }

/** smooth pseudo-random function of time, -1..1, different per seed */
inline float smoothNoise (double t, uint32_t seed) noexcept
{
    XorShift r (seed * 2654435761u + 12345u);
    const double f1 = 0.55 + 0.5 * r.uni(), f2 = 1.13 + 0.7 * r.uni(), f3 = 0.31 + 0.3 * r.uni();
    const double p1 = 6.283 * r.uni(), p2 = 6.283 * r.uni(), p3 = 6.283 * r.uni();
    const double two_pi = juce::MathConstants<double>::twoPi;
    return (float) ((std::sin (two_pi * f1 * t + p1) + 0.6 * std::sin (two_pi * f2 * t + p2)
                     + 0.8 * std::sin (two_pi * f3 * t + p3)) / 2.4);
}

inline Pos applyMotion (Pos base, Motion mode, float rateHz, float depthDeg, double t, uint32_t seed) noexcept
{
    switch (mode)
    {
        case Motion::Rotation:
            base.az = wrapAz (base.az + (float) std::fmod (360.0 * rateHz * t, 360.0));
            break;
        case Motion::Pendulum:
            base.az = wrapAz (base.az + depthDeg * (float) std::sin (juce::MathConstants<double>::twoPi * rateHz * t));
            break;
        case Motion::Drift:
            base.az = wrapAz (base.az + depthDeg * smoothNoise (rateHz * t, seed));
            base.el = juce::jlimit (-90.0f, 90.0f, base.el + 0.4f * depthDeg * smoothNoise (rateHz * t, seed + 7777u));
            break;
        case Motion::Static:
        default: break;
    }
    return base;
}

// --------------------------------------------------------------------------
// Layout generators (UI side, write the base position of every pipe of a stop)
// --------------------------------------------------------------------------
enum class Layout { Case = 0, Pitch, Scatter, Ring, Spiral, Zone };

inline juce::StringArray layoutNames()
{
    return { juce::String (juce::CharPointer_UTF8 ("Geh\xc3\xa4use C/C\xe2\x99\xaf")), juce::String (juce::CharPointer_UTF8 ("Tonh\xc3\xb6he")),
             "Streuung", "Ring", "Spirale", "Zone" };
}

struct LayoutSettings
{
    int   type = 0;
    float width = 120.0f;      // degrees
    float center = 0.0f;       // azimuth of the centre
    float elevation = 0.0f;
    float spread = 10.0f;      // elevation spread
};

inline void generateLayout (const LayoutSettings& s, int stopIndex, std::array<Pos, kNumKeys>& out)
{
    const auto byPitch = keysByPitch();
    XorShift rnd ((uint32_t) (1234 + stopIndex * 977 + s.type * 31));
    const float half = s.width * 0.5f;

    // rank 0..1 by pitch for each key index
    std::array<float, kNumKeys> rank {};
    for (int i = 0; i < kNumKeys; ++i) rank[(size_t) byPitch[(size_t) i]] = (float) i / (float) (kNumKeys - 1);

    for (int k = 0; k < kNumKeys; ++k)
    {
        const float r = rank[(size_t) k];
        Pos p { s.center, s.elevation };
        switch ((Layout) s.type)
        {
            case Layout::Case:
            {
                // C side / C# side as in an organ case: alternate semitones left/right,
                // bass in the middle, treble towards the outside
                const bool cSide = (kPhysKeys[(size_t) k] % 2) == 0;
                p.az = s.center + (cSide ? 1.0f : -1.0f) * half * (0.06f + 0.94f * r);
                p.el = s.elevation + s.spread * (0.5f - r) * 0.5f;
                break;
            }
            case Layout::Pitch:   p.az = s.center + half - r * s.width; p.el = s.elevation; break;
            case Layout::Scatter: p.az = s.center + rnd.bi() * half;   p.el = s.elevation + rnd.bi() * s.spread; break;
            case Layout::Ring:    p.az = s.center + 360.0f * r;        p.el = s.elevation; break;
            case Layout::Spiral:  p.az = s.center + 720.0f * r;        p.el = s.elevation - s.spread + 2.0f * s.spread * r; break;
            case Layout::Zone:    p.az = s.center + rnd.bi() * half * 0.15f; p.el = s.elevation + rnd.bi() * s.spread * 0.3f; break;
            default: break;
        }
        p.az = wrapAz (p.az);
        p.el = juce::jlimit (-90.0f, 90.0f, p.el);
        out[(size_t) k] = p;
    }
}

/** the layout every stop starts with */
inline LayoutSettings defaultLayout (int stopIndex)
{
    LayoutSettings s;
    s.type = 0;
    s.width = 110.0f + 8.0f * (float) stopIndex;
    s.center = 0.0f;
    s.elevation = 4.0f + 7.0f * (float) stopIndex;
    s.spread = 10.0f;
    return s;
}
} // namespace rysum
