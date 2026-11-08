#pragma once

#include "Common.h"

namespace rysum
{
/** Position in degrees + distance in metres. Azimuth: 0 = front, +90 = left (AmbiX convention).
    Elevation: 0 = horizon, +90 = zenith. */
struct Pos { float az = 0.0f, el = 0.0f, dist = 6.0f; };

constexpr float kMinDist = 0.5f, kMaxDist = 60.0f, kRefDist = 4.0f;   // gain 1 at kRefDist

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
namespace detail
{
    /** SN3D normalisation sqrt ((2 - delta_m) (l - m)! / (l + m)!), [l][m], m >= 0 */
    struct Sn3dTable
    {
        double n[kMaxOrder + 1][kMaxOrder + 1] {};
        Sn3dTable() noexcept
        {
            double fact[2 * kMaxOrder + 2] {};
            fact[0] = 1.0;
            for (int i = 1; i < 2 * kMaxOrder + 2; ++i) fact[i] = fact[i - 1] * i;
            for (int l = 0; l <= kMaxOrder; ++l)
                for (int m = 0; m <= l; ++m)
                    n[l][m] = std::sqrt ((m == 0 ? 1.0 : 2.0) * fact[l - m] / fact[l + m]);
        }
    };
    inline const Sn3dTable& sn3d() noexcept { static const Sn3dTable t; return t; }
}

/** the same encoder for a direction given by its cos / sin of azimuth and elevation (no trigonometry at all) */
inline void encodeAmbiXDir (double cosAz, double sinAz, double sinEl, double cosEl, int order, float* out) noexcept
{
    const double x = sinEl, c = cosEl;
    double P[kMaxOrder + 1][kMaxOrder + 1];
    P[0][0] = 1.0;
    for (int m = 1; m <= order; ++m)
        P[m][m] = (2 * m - 1) * c * P[m - 1][m - 1];
    for (int m = 0; m < order; ++m)
        P[m + 1][m] = x * (2 * m + 1) * P[m][m];
    for (int m = 0; m <= order; ++m)
        for (int l = m + 2; l <= order; ++l)
            P[l][m] = ((2 * l - 1) * x * P[l - 1][m] - (l + m - 1) * P[l - 2][m]) / (double) (l - m);
    double cm[kMaxOrder + 1], sm[kMaxOrder + 1];
    cm[0] = 1.0; sm[0] = 0.0;
    if (order >= 1) { cm[1] = cosAz; sm[1] = sinAz; }
    for (int m = 2; m <= order; ++m)
    {
        cm[m] = cm[m - 1] * cm[1] - sm[m - 1] * sm[1];
        sm[m] = sm[m - 1] * cm[1] + cm[m - 1] * sm[1];
    }
    const auto& N = detail::sn3d().n;
    for (int l = 0; l <= order; ++l)
    {
        float* o = out + l * l + l;
        o[0] = (float) (N[l][0] * P[l][0]);
        for (int m = 1; m <= l; ++m)
        {
            const double a = N[l][m] * P[l][m];
            o[m]  = (float) (a * cm[m]);
            o[-m] = (float) (a * sm[m]);
        }
    }
}

inline void encodeAmbiX (float azDeg, float elDeg, int order, float* out) noexcept
{
    // (called for every pipe every 64 samples and for every image source of the simulated room: two sincos,
    // the rest are recurrences and a table)
    const double az = azDeg * juce::MathConstants<double>::pi / 180.0;
    const double el = elDeg * juce::MathConstants<double>::pi / 180.0;
    const double x  = std::sin (el);                 // argument of the Legendre functions
    const double c  = std::cos (el);                 // sqrt(1 - x^2)

    // associated Legendre P[l][m] without CS phase
    double P[kMaxOrder + 1][kMaxOrder + 1];
    P[0][0] = 1.0;
    for (int m = 1; m <= order; ++m)
        P[m][m] = (2 * m - 1) * c * P[m - 1][m - 1];
    for (int m = 0; m < order; ++m)
        P[m + 1][m] = x * (2 * m + 1) * P[m][m];
    for (int m = 0; m <= order; ++m)
        for (int l = m + 2; l <= order; ++l)
            P[l][m] = ((2 * l - 1) * x * P[l - 1][m] - (l + m - 1) * P[l - 2][m]) / (double) (l - m);

    // cos (m az), sin (m az) by the angle-addition recurrence
    double cm[kMaxOrder + 1], sm[kMaxOrder + 1];
    cm[0] = 1.0; sm[0] = 0.0;
    if (order >= 1) { cm[1] = std::cos (az); sm[1] = std::sin (az); }
    for (int m = 2; m <= order; ++m)
    {
        cm[m] = cm[m - 1] * cm[1] - sm[m - 1] * sm[1];
        sm[m] = sm[m - 1] * cm[1] + cm[m - 1] * sm[1];
    }

    const auto& N = detail::sn3d().n;
    for (int l = 0; l <= order; ++l)
    {
        float* o = out + l * l + l;                  // channel of m = 0
        o[0] = (float) (N[l][0] * P[l][0]);
        for (int m = 1; m <= l; ++m)
        {
            const double a = N[l][m] * P[l][m];
            o[m]  = (float) (a * cm[m]);
            o[-m] = (float) (a * sm[m]);
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
enum class Motion { Static = 0, Rotation, Pendulum, Drift, Vertical, Jumps, Drawn };

inline juce::StringArray motionNames()
{
    // new entries only at the end: projects and patches store the index
    return { "Statisch", "Rotation", "Pendel", "Drift", "Vertikal", juce::String::fromUTF8 ("Spr\xc3\xbcnge"), "Gezeichnet" };
}

/** a drawn trajectory (Raum page): points on the sphere in the order (and timing) they were drawn */
struct MotionPath
{
    static constexpr int kMax = 256;
    int n = 0;                                  // points (0 = no path)
    std::array<float, kMax> x {}, y {}, z {};   // unit vectors: x = front, y = left, z = up
    std::array<float, kMax> u {};               // 0..1: where along one pass the point is reached (time)
    bool pingPong = false;                      // false: a loop (closes back to the start), true: there and back

    /** direction at u (0..1) */
    void at (float uu, float& az, float& el) const noexcept
    {
        if (n <= 0) { az = 0.0f; el = 0.0f; return; }
        int lo = 0, hi = n - 1;
        if (uu <= u[0]) hi = 0;
        else if (uu >= u[(size_t) (n - 1)]) lo = n - 1;
        else while (hi - lo > 1) { const int m = (lo + hi) / 2; if (u[(size_t) m] <= uu) lo = m; else hi = m; }
        float vx = x[(size_t) lo], vy = y[(size_t) lo], vz = z[(size_t) lo];
        if (hi != lo)
        {
            const float t = (uu - u[(size_t) lo]) / juce::jmax (1.0e-6f, u[(size_t) hi] - u[(size_t) lo]);
            vx += t * (x[(size_t) hi] - vx); vy += t * (y[(size_t) hi] - vy); vz += t * (z[(size_t) hi] - vz);
        }
        az = juce::radiansToDegrees (std::atan2 (vy, vx));
        el = juce::radiansToDegrees (std::atan2 (vz, std::sqrt (vx * vx + vy * vy)));
    }
};

/** the shape of an orbit / path, per stop */
struct MotionShape
{
    float ellipse = 0.0f;         // 0..0.9: how much nearer the near side is
    float ellipseDir = 0.0f;      // direction of the near side (deg, 0 = front)
    float ellipseTilt = 0.0f;     // orbit tilted: up to this much higher at the near side, lower at the far side (deg)
    int   ellipseLobes = 1;       // 1 = listener at a focus (near on one side), 2 = in the centre (near front and back), 3, 4
    const MotionPath* path = nullptr;
    float pathSpread = 1.0f;      // drawn path: 0 = all pipes at one point, 1 = spread over the whole path
    float keyFrac = 0.0f;         // 0..1: where this pipe is among the stop's pipes
};

inline juce::StringArray ellipseFormNames()
{
    return { "Brennpunkt", "Mitte", "Dreieck", "Viereck" };
}

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

/** jumps: every pipe jumps at its own moment within a cycle (0..1) */
inline double jumpOffset (uint32_t seed) noexcept { XorShift off (seed * 31u + 7u); return off.uni(); }

/** Moves a pipe. 'phase' is the integrated motion phase in cycles (speed in Hz integrated
    over time by the engine), so changing the speed never makes the position jump;
    a negative speed simply runs the phase backwards (other direction). */
inline Pos applyMotion (Pos base, Motion mode, double phase, float depthDeg, uint32_t seed, const MotionShape& shape = {}) noexcept
{
    switch (mode)
    {
        case Motion::Rotation:
            base.az = wrapAz (base.az + (float) (360.0 * (phase - std::floor (phase))));
            break;
        case Motion::Vertical:
        {
            // turn around the left-right axis: front -> up -> back -> down
            const double a = juce::MathConstants<double>::twoPi * (phase - std::floor (phase));
            const double az = juce::degreesToRadians ((double) base.az), el = juce::degreesToRadians ((double) base.el);
            const double x = std::cos (el) * std::cos (az), y = std::cos (el) * std::sin (az), z = std::sin (el);
            const double x2 = x * std::cos (a) - z * std::sin (a);
            const double z2 = x * std::sin (a) + z * std::cos (a);
            base.az = (float) juce::radiansToDegrees (std::atan2 (y, x2));
            base.el = (float) juce::radiansToDegrees (std::asin (juce::jlimit (-1.0, 1.0, z2)));
            break;
        }
        case Motion::Pendulum:
            base.az = wrapAz (base.az + depthDeg * (float) std::sin (juce::MathConstants<double>::twoPi * phase));
            break;
        case Motion::Drift:
            base.az = wrapAz (base.az + depthDeg * smoothNoise (phase, seed));
            base.el = juce::jlimit (-90.0f, 90.0f, base.el + 0.4f * depthDeg * smoothNoise (phase, seed + 7777u));
            base.dist *= 1.0f + 0.4f * (depthDeg / 180.0f) * smoothNoise (phase * 0.7, seed + 999u);
            break;
        case Motion::Jumps:
        {
            // sudden jumps to random places around the base position; every pipe jumps at its own moment
            const double idx = std::floor (phase + jumpOffset (seed));
            XorShift r ((uint32_t) (seed * 7919u) ^ (uint32_t) (int64_t) idx * 104729u);
            r.next(); r.next();
            base.az = wrapAz (base.az + depthDeg * r.bi());
            base.el = juce::jlimit (-90.0f, 90.0f, base.el + 0.5f * depthDeg * r.bi());
            base.dist *= 1.0f + 0.6f * (depthDeg / 180.0f) * r.bi();
            break;
        }
        case Motion::Drawn:
        {
            // along the drawn path; the pipes of the stop follow each other along it like a train
            if (shape.path != nullptr && shape.path->n > 0)
            {
                double t = phase + (double) (shape.pathSpread * shape.keyFrac);
                t -= std::floor (t);
                const float u = shape.path->pingPong ? 1.0f - std::abs (1.0f - 2.0f * (float) t) : (float) t;
                shape.path->at (u, base.az, base.el);
            }
            break;
        }
        case Motion::Static:
        default: break;
    }
    if (shape.ellipse > 0.0f || shape.ellipseTilt != 0.0f)
    {
        const float rel = juce::degreesToRadians (base.az - shape.ellipseDir), e = juce::degreesToRadians (base.el);
        // the orbit leans: higher towards the near side, lower on the far side
        if (shape.ellipseTilt != 0.0f)
            base.el = juce::jlimit (-90.0f, 90.0f, base.el + shape.ellipseTilt * std::cos (rel) * std::cos (e));
        // elliptic orbit: nearer on the near side(s); 1 lobe = the listener at a focus, 2 = in the centre, 3 / 4 = triangle / square
        if (shape.ellipse > 0.0f)
            base.dist *= 1.0f - shape.ellipse * std::cos (e) * std::cos ((float) juce::jlimit (1, 4, shape.ellipseLobes) * rel);
    }
    base.dist = juce::jlimit (kMinDist, kMaxDist, base.dist);
    return base;
}

/** gain from distance (1 at kRefDist), 'amount' 0 = no distance effect, 1 = physical 1/r */
inline float distanceGain (float dist, float amount) noexcept
{
    return juce::jlimit (0.0f, 4.0f, std::pow (kRefDist / juce::jmax (kMinDist, dist), amount));
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
    float distance = 6.0f;     // metres
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
        Pos p { s.center, s.elevation, s.distance };
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
            case Layout::Scatter: p.az = s.center + rnd.bi() * half;   p.el = s.elevation + rnd.bi() * s.spread; p.dist *= 1.0f + 0.45f * rnd.bi(); break;
            case Layout::Ring:    p.az = s.center + 360.0f * r;        p.el = s.elevation; break;
            case Layout::Spiral:  p.az = s.center + 720.0f * r;        p.el = s.elevation - s.spread + 2.0f * s.spread * r; p.dist *= 1.5f - r; break;
            case Layout::Zone:    p.az = s.center + rnd.bi() * half * 0.15f; p.el = s.elevation + rnd.bi() * s.spread * 0.3f; break;
            default: break;
        }
        p.az = wrapAz (p.az);
        p.el = juce::jlimit (-90.0f, 90.0f, p.el);
        p.dist = juce::jlimit (kMinDist, kMaxDist, p.dist);
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
    s.distance = 5.0f + 0.5f * (float) stopIndex;
    if (stopIndex == kPraestant16) { s.width = 150.0f; s.elevation = 0.0f; s.distance = 7.0f; }   // big pipes: low and wide
    return s;
}
} // namespace rysum
