#pragma once

#include "Common.h"
#include "Spatial.h"
#include <array>
#include <cmath>
#include <vector>

namespace rysum
{
/** "Saal": a simulated shoebox room.
    Coordinates in metres: x = to the right (0 = left wall), y = forward (0 = back wall, length = front wall),
    z = up (0 = floor). The listener faces +y. Directions use the plugin's convention (azimuth counter-clockwise,
    i.e. +90 = left). */
struct RoomParams
{
    bool  on = false;
    float width = 14.0f, length = 28.0f, height = 11.0f, size = 1.0f;   // size scales all three
    float listenerX = 0.5f, listenerY = 0.4f;                            // fractions of width / length
    float absWalls = 0.10f, absFloor = 0.25f;                            // absorption (floor = floor + ceiling), mid / low frequencies
    float highDamp = 0.5f;                                               // extra absorption of the highs
    float scatter = 0.3f;                                                // part of the energy reflected diffusely
    int   order = 2;                                                     // early reflections up to this order (1..3)
    float erGain = 1.0f, tailGain = 1.0f;                                // linear

    float W() const noexcept { return juce::jmax (1.0f, width * size); }
    float L() const noexcept { return juce::jmax (1.0f, length * size); }
    float H() const noexcept { return juce::jmax (1.0f, height * size); }
    static constexpr float kEarHeight = 1.6f;
};

constexpr float kSpeedOfSoundRoom = 343.0f;

struct Vec3f { float x = 0, y = 0, z = 0; };

/** absorption of walls / floor+ceiling in the two bands (low/mid, high) */
struct BandAbsorption
{
    float wallLow, wallHigh, floorLow, floorHigh;
    static BandAbsorption from (const RoomParams& r) noexcept
    {
        auto hi = [&] (float a) { return juce::jlimit (0.0f, 0.99f, a + r.highDamp * (1.0f - a) * 0.6f); };
        const float w = juce::jlimit (0.0f, 0.99f, r.absWalls), f = juce::jlimit (0.0f, 0.99f, r.absFloor);
        return { w, hi (w), f, hi (f) };
    }
};

/** air absorption (energy, per metre) in the two bands */
inline float airLow() noexcept  { return 0.0002f; }
inline float airHigh (float air) noexcept { return 0.004f + 0.012f * air; }

inline Vec3f listenerPos (const RoomParams& r) noexcept
{
    return { juce::jlimit (0.3f, r.W() - 0.3f, r.listenerX * r.W()),
             juce::jlimit (0.3f, r.L() - 0.3f, r.listenerY * r.L()),
             juce::jmin (RoomParams::kEarHeight, r.H() * 0.5f) };
}

/** a direction / distance seen from the listener -> point in the room (kept 0.3 m inside the walls) */
inline Vec3f roomPoint (const RoomParams& r, Pos p) noexcept
{
    const auto L = listenerPos (r);
    const float a = juce::degreesToRadians (p.az), e = juce::degreesToRadians (p.el);
    const float d = p.dist;
    return { juce::jlimit (0.3f, r.W() - 0.3f, L.x - std::sin (a) * std::cos (e) * d),
             juce::jlimit (0.3f, r.L() - 0.3f, L.y + std::cos (a) * std::cos (e) * d),
             juce::jlimit (0.3f, r.H() - 0.3f, L.z + std::sin (e) * d) };
}

/** a point -> direction / distance from the listener */
inline Pos seenFrom (const Vec3f& L, const Vec3f& q) noexcept
{
    const float dx = q.x - L.x, dy = q.y - L.y, dz = q.z - L.z;
    const float r = std::sqrt (dx * dx + dy * dy + dz * dz);
    Pos p;
    p.az = juce::radiansToDegrees (std::atan2 (-dx, dy));
    p.el = r > 1.0e-6f ? juce::radiansToDegrees (std::asin (juce::jlimit (-1.0f, 1.0f, dz / r))) : 0.0f;
    p.dist = r;
    return p;
}

/** one mirror image of a source */
struct ImageSource
{
    Vec3f pos;
    int hitsWalls = 0, hitsFloor = 0;      // reflections on the side/front/back walls, on floor + ceiling
    int mx = 0, my = 0, mz = 0;
};

/** image positions along one axis: m reflections, room [0, D], source at s */
inline float imageCoord (int m, float D, float s) noexcept
{
    return (float) m * D + ((m % 2 == 0) ? s : D - s);
}

/** all images up to 'order' reflections (order 1: 6, 2: 24, 3: 62) */
inline int imageSources (const RoomParams& r, const Vec3f& src, int order, ImageSource* out, int maxOut) noexcept
{
    int n = 0;
    order = juce::jlimit (1, 3, order);
    for (int mx = -order; mx <= order; ++mx)
        for (int my = -order; my <= order; ++my)
            for (int mz = -order; mz <= order; ++mz)
            {
                const int k = std::abs (mx) + std::abs (my) + std::abs (mz);
                if (k == 0 || k > order || n >= maxOut) continue;
                ImageSource& im = out[n++];
                im.pos = { imageCoord (mx, r.W(), src.x), imageCoord (my, r.L(), src.y), imageCoord (mz, r.H(), src.z) };
                im.hitsWalls = std::abs (mx) + std::abs (my);
                im.hitsFloor = std::abs (mz);
                im.mx = mx; im.my = my; im.mz = mz;
            }
    return n;
}

/** Eyring decay time (s) of the room in one band */
inline float eyringRt (const RoomParams& r, bool high, float air) noexcept
{
    const auto a = BandAbsorption::from (r);
    const float W = r.W(), L = r.L(), H = r.H();
    const float V = W * L * H;
    const float Sw = 2.0f * (W * H + L * H), Sf = 2.0f * W * L;
    const float S = Sw + Sf;
    const float am = ((high ? a.wallHigh : a.wallLow) * Sw + (high ? a.floorHigh : a.floorLow) * Sf) / S;
    const float m = high ? airHigh (air) : airLow();
    const float denom = -S * std::log (juce::jmax (1.0e-4f, 1.0f - am)) + 4.0f * m * V;
    return juce::jlimit (0.05f, 30.0f, 0.161f * V / juce::jmax (1.0e-4f, denom));
}

inline float roomVolume (const RoomParams& r) noexcept { return r.W() * r.L() * r.H(); }
inline float meanFreePath (const RoomParams& r) noexcept
{
    const float S = 2.0f * (r.W() * r.L() + r.W() * r.H() + r.L() * r.H());
    return 4.0f * roomVolume (r) / S;
}

/** Ray tracing (energy, two bands): rays leave a source, reflect specularly or (with probability 'scatter')
    diffusely (Lambert), lose energy at every wall and in the air. The energy left in the whole ray
    ensemble over time is the room's decay; its slope between -5 and -35 dB gives T30. This catches
    what Eyring cannot: uneven absorption (e.g. a dead floor and hard walls), long or flat rooms where the
    sound field is not diffuse, and how scattering mixes the energy. */
struct RayResult
{
    float rtLow = 1.0f, rtHigh = 1.0f;     // s
    int rays = 0, reflections = 0;
    bool valid = false;
};

inline RayResult traceRoom (const RoomParams& r, float air, int numRays = 3000, uint32_t seed = 12345)
{
    RayResult res;
    const auto ab = BandAbsorption::from (r);
    const float W = r.W(), L = r.L(), H = r.H();
    constexpr float binSec = 0.005f, maxSec = 30.0f;
    const int bins = (int) (maxSec / binSec);
    std::vector<double> eLow ((size_t) bins, 0.0), eHigh ((size_t) bins, 0.0);
    XorShift rng (seed);
    const float mL = airLow(), mH = airHigh (air);
    const Vec3f src { W * 0.5f, L * 0.75f, juce::jmin (H * 0.5f, 4.0f) };   // roughly where an organ stands
    int totalRefl = 0;
    for (int ray = 0; ray < numRays; ++ray)
    {
        // uniform direction
        const float z = rng.bi(), phi = juce::MathConstants<float>::twoPi * rng.uni();
        const float s = std::sqrt (juce::jmax (0.0f, 1.0f - z * z));
        Vec3f d { s * std::cos (phi), s * std::sin (phi), z };
        Vec3f p = src;
        double el = 1.0, eh = 1.0, t = 0.0;
        for (int hit = 0; hit < 4000 && t < maxSec && (el > 1.0e-7 || eh > 1.0e-7); ++hit)
        {
            // distance to the next wall
            float tx = 1e30f, ty = 1e30f, tz = 1e30f;
            if (d.x > 1e-9f) tx = (W - p.x) / d.x; else if (d.x < -1e-9f) tx = -p.x / d.x;
            if (d.y > 1e-9f) ty = (L - p.y) / d.y; else if (d.y < -1e-9f) ty = -p.y / d.y;
            if (d.z > 1e-9f) tz = (H - p.z) / d.z; else if (d.z < -1e-9f) tz = -p.z / d.z;
            const float dist = juce::jmax (1.0e-4f, juce::jmin (tx, ty, tz));
            const int axis = tx <= ty && tx <= tz ? 0 : (ty <= tz ? 1 : 2);
            // energy along the segment (air absorption inside the segment is folded in at its end)
            const double t1 = t + dist / kSpeedOfSoundRoom;
            const int b0 = (int) (t / binSec), b1 = juce::jmin (bins - 1, (int) (t1 / binSec));
            for (int b = b0; b <= b1 && b < bins; ++b)
            {
                const double lo = juce::jmax (t, (double) b * binSec), hi = juce::jmin (t1, (double) (b + 1) * binSec);
                if (hi > lo) { eLow[(size_t) b] += el * (hi - lo); eHigh[(size_t) b] += eh * (hi - lo); }
            }
            t = t1;
            p = { p.x + d.x * dist, p.y + d.y * dist, p.z + d.z * dist };
            el *= std::exp (-mL * dist); eh *= std::exp (-mH * dist);
            const bool floor = axis == 2;
            el *= 1.0 - (floor ? ab.floorLow : ab.wallLow);
            eh *= 1.0 - (floor ? ab.floorHigh : ab.wallHigh);
            ++totalRefl;
            // new direction: specular, or Lambert around the wall normal
            Vec3f nrm { 0, 0, 0 };
            if (axis == 0) nrm.x = d.x > 0 ? -1.0f : 1.0f;
            else if (axis == 1) nrm.y = d.y > 0 ? -1.0f : 1.0f;
            else nrm.z = d.z > 0 ? -1.0f : 1.0f;
            if (rng.uni() < r.scatter)
            {
                // cosine-weighted hemisphere
                const float u1 = rng.uni(), u2 = rng.uni();
                const float rr = std::sqrt (u1), th = juce::MathConstants<float>::twoPi * u2;
                const float a = rr * std::cos (th), b = rr * std::sin (th), c = std::sqrt (juce::jmax (0.0f, 1.0f - u1));
                if (axis == 0)      d = { nrm.x * c, a, b };
                else if (axis == 1) d = { a, nrm.y * c, b };
                else                d = { a, b, nrm.z * c };
            }
            else
            {
                if (axis == 0) d.x = -d.x; else if (axis == 1) d.y = -d.y; else d.z = -d.z;
            }
            // keep the point inside
            p.x = juce::jlimit (1.0e-4f, W - 1.0e-4f, p.x);
            p.y = juce::jlimit (1.0e-4f, L - 1.0e-4f, p.y);
            p.z = juce::jlimit (1.0e-4f, H - 1.0e-4f, p.z);
        }
    }
    // decay time from the ensemble energy: least-squares line through -5 .. -35 dB of the backward-integrated curve
    auto fit = [&] (const std::vector<double>& e)
    {
        std::vector<double> sch ((size_t) bins);
        double acc = 0;
        for (int b = bins - 1; b >= 0; --b) { acc += e[(size_t) b]; sch[(size_t) b] = acc; }
        if (acc <= 0) return 30.0f;
        double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
        for (int b = 0; b < bins; ++b)
        {
            const double db = 10.0 * std::log10 (juce::jmax (1e-30, sch[(size_t) b] / acc));
            if (db > -5.0) continue;
            if (db < -35.0) break;
            const double x = b * binSec;
            sx += x; sy += db; sxx += x * x; sxy += x * db; ++n;
        }
        if (n < 3) return 30.0f;
        const double slope = (n * sxy - sx * sy) / juce::jmax (1e-12, n * sxx - sx * sx);    // dB per second
        return slope < -1e-3 ? juce::jlimit (0.05f, 30.0f, (float) (-60.0 / slope)) : 30.0f;
    };
    res.rtLow = fit (eLow);
    res.rtHigh = fit (eHigh);
    res.rays = numRays;
    res.reflections = totalRefl;
    res.valid = true;
    return res;
}
} // namespace rysum
