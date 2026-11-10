#pragma once

#include "Common.h"
#include "Spatial.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace rysum
{
/** "Saal": a simulated shoebox room.
    Coordinates in metres: x = to the right (0 = left wall), y = forward (0 = back wall, length =
   front wall), z = up (0 = floor). The listener faces +y. Directions use the plugin's convention
   (azimuth counter-clockwise, i.e. +90 = left). */
/** floor plans: the room is a prism - a polygon (the walls, all vertical) between a flat floor and
   a flat ceiling. Shape 0 is the shoebox (the original code, unchanged); the others are polygons in
   fractions of width / length, counter-clockwise (x to the right, y to the front). */
constexpr int kMaxPlanVerts = 12;
enum RoomShape
{
    shapeBox = 0,
    shapeL,
    shapeFan,
    shapeHex,
    shapeOct,
    shapeRound,
    shapeCross,
    shapeApse,
    shapeDrawn,
    kNumRoomShapes
};

struct FloorPlan
{
    int n = 4;
    std::array<float, kMaxPlanVerts> x{0.0f, 1.0f, 1.0f, 0.0f}, y{0.0f, 0.0f, 1.0f, 1.0f};
    bool operator==(const FloorPlan &o) const noexcept
    {
        if (n != o.n)
            return false;
        // Exact equality detects changes to stored coordinates, not geometric proximity.
        return std::equal(x.begin(), x.begin() + n, o.x.begin()) &&
               std::equal(y.begin(), y.begin() + n, o.y.begin());
    }
};

struct RoomParams
{
    bool on = false;
    int shape = shapeBox;  // RoomShape
    float shapeAmt = 0.5f; // how strongly the shape departs from the shoebox (0..1)
    FloorPlan plan;        // filled by finishPlan() (fractions of width / length)
    bool box() const noexcept
    {
        return shape == shapeBox;
    }
    float width = 14.0f, length = 28.0f, height = 11.0f, size = 1.0f; // size scales all three
    float listenerX = 0.5f, listenerY = 0.4f;                         // fractions of width / length
    float absWalls = 0.10f,
          absFloor = 0.25f; // absorption (floor = floor + ceiling), mid / low frequencies
    float highDamp = 0.5f;  // extra absorption of the highs
    float scatter = 0.3f;   // part of the energy reflected diffusely
    int order = 2;          // early reflections up to this order (1..3)
    float erGain = 1.0f, tailGain = 1.0f; // linear
    int tailModel = 0; // late reverb: 0 = convolution (ray-traced response), 1 = FDN

    float W() const noexcept
    {
        return juce::jmax(1.0f, width * size);
    }
    float L() const noexcept
    {
        return juce::jmax(1.0f, length * size);
    }
    float H() const noexcept
    {
        return juce::jmax(1.0f, height * size);
    }
    static constexpr float kEarHeight = 1.6f;
};

constexpr float kSpeedOfSoundRoom = 343.0f;
constexpr int kNumRooms =
    4; // simulated rooms ("Saal 1..4"): every stop (and the input) sounds in one of them

struct Vec3f
{
    float x = 0, y = 0, z = 0;
};

/** absorption of walls / floor+ceiling in the two bands (low/mid, high) */
struct BandAbsorption
{
    float wallLow, wallHigh, floorLow, floorHigh;
    static BandAbsorption from(const RoomParams &r) noexcept
    {
        auto hi = [&](float a)
        { return juce::jlimit(0.0f, 0.99f, a + r.highDamp * (1.0f - a) * 0.6f); };
        const float w = juce::jlimit(0.0f, 0.99f, r.absWalls),
                    f = juce::jlimit(0.0f, 0.99f, r.absFloor);
        return {w, hi(w), f, hi(f)};
    }
};

/** air absorption (energy, per metre) in the two bands */
inline float airLow() noexcept
{
    return 0.0002f;
}
inline float airHigh(float air) noexcept
{
    return 0.004f + 0.012f * air;
}

/** image positions along one axis: m reflections, room [0, D], source at s */
inline float imageCoord(int m, float D, float s) noexcept
{
    return (float)m * D + ((m % 2 == 0) ? s : D - s);
}

// ---------------------------------------------------------------------------
// floor plans
// ---------------------------------------------------------------------------
/** the plan of a shape. 'a' (0..1) = how far it departs from the rectangle; 'drawn' = the user's
 * own plan */
inline FloorPlan makeFloorPlan(int shape, float a, const FloorPlan &drawn) noexcept
{
    a = juce::jlimit(0.0f, 1.0f, a);
    FloorPlan f;
    f.n = 0;
    auto add = [&f](float x, float y)
    {
        if (f.n < kMaxPlanVerts)
        {
            f.x[(size_t)f.n] = x;
            f.y[(size_t)f.n] = y;
            ++f.n;
        }
    };
    const float pi = juce::MathConstants<float>::pi;
    switch (shape)
    {
    case shapeL: // a corner cut away (front right): the organ looks around the corner
    {
        const float c = 0.15f + 0.3f * a;
        add(0, 0);
        add(1, 0);
        add(1, 1 - c);
        add(1 - c, 1 - c);
        add(1 - c, 1);
        add(0, 1);
        break;
    }
    case shapeFan: // narrow at the front (the organ), wide at the back: a fan-shaped hall
    {
        const float w = 1.0f - 0.75f * a;
        add(0, 0);
        add(1, 0);
        add(0.5f + 0.5f * w, 1);
        add(0.5f - 0.5f * w, 1);
        break;
    }
    case shapeHex:
    {
        const float h = 0.08f + 0.34f * a;
        add(h, 0);
        add(1 - h, 0);
        add(1, 0.5f);
        add(1 - h, 1);
        add(h, 1);
        add(0, 0.5f);
        break;
    }
    case shapeOct:
    {
        const float c = 0.06f + 0.3f * a;
        add(c, 0);
        add(1 - c, 0);
        add(1, c);
        add(1, 1 - c);
        add(1 - c, 1);
        add(c, 1);
        add(0, 1 - c);
        add(0, c);
        break;
    }
    case shapeRound: // rounded rectangle (a = 0) .. ellipse (a = 1), twelve flat walls: concave, it
                     // focuses
    {
        const float pexp = 2.0f + 8.0f * (1.0f - a) * (1.0f - a);
        for (int i = 0; i < 12; ++i)
        {
            const float t = 2.0f * pi * ((float)i + 0.5f) / 12.0f - 0.5f * pi;
            const float c = std::cos(t), sn = std::sin(t);
            add(0.5f + 0.5f * std::copysign(std::pow(std::abs(c), 2.0f / pexp), c),
                0.5f + 0.5f * std::copysign(std::pow(std::abs(sn), 2.0f / pexp), sn));
        }
        break;
    }
    case shapeCross: // a cruciform church: nave along the length, a transept in front of the
                     // listener
    {
        const float nw = 0.5f * (0.75f - 0.4f * a), t0 = 0.55f, t1 = 0.8f, x0 = 0.5f - nw,
                    x1 = 0.5f + nw;
        add(x0, 0);
        add(x1, 0);
        add(x1, t0);
        add(1, t0);
        add(1, t1);
        add(x1, t1);
        add(x1, 1);
        add(x0, 1);
        add(x0, t1);
        add(0, t1);
        add(0, t0);
        add(x0, t0);
        break;
    }
    case shapeApse: // a nave with a rounded apse at the front
    {
        const float w = 0.45f + 0.55f * a, d = 0.22f;
        add(0, 0);
        add(1, 0);
        if (w < 0.995f)
        {
            add(1, 1 - d);
        }
        add(0.5f + 0.5f * w, 1 - d);
        for (int k = 1; k <= 5; ++k)
        {
            const float ph = pi * (float)k / 6.0f;
            add(0.5f + 0.5f * w * std::cos(ph), 1 - d + d * std::sin(ph));
        }
        add(0.5f - 0.5f * w, 1 - d);
        if (w < 0.995f)
        {
            add(0, 1 - d);
        }
        break;
    }
    case shapeDrawn:
    {
        for (int i = 0; i < juce::jmin(drawn.n, kMaxPlanVerts); ++i)
            add(juce::jlimit(0.0f, 1.0f, drawn.x[(size_t)i]),
                juce::jlimit(0.0f, 1.0f, drawn.y[(size_t)i]));
        double area = 0.0;
        for (int i = 0; i < f.n; ++i)
        {
            const int j = (i + 1) % f.n;
            area +=
                (double)f.x[(size_t)i] * f.y[(size_t)j] - (double)f.x[(size_t)j] * f.y[(size_t)i];
        }
        if (f.n < 3 || std::abs(area) < 1.0e-3)
        {
            f = FloorPlan();
            break;
        }
        if (area < 0.0)
            for (int i = 0, j = f.n - 1; i < j; ++i, --j)
            {
                std::swap(f.x[(size_t)i], f.x[(size_t)j]);
                std::swap(f.y[(size_t)i], f.y[(size_t)j]);
            }
        break;
    }
    default:
        f = FloorPlan();
        break;
    }
    return f;
}

/** the user's own plan before anything was drawn */
inline FloorPlan defaultDrawnPlan() noexcept
{
    FloorPlan f;
    const float xs[]{0.12f, 0.88f, 1.0f, 0.72f, 0.18f, 0.0f},
        ys[]{0.0f, 0.06f, 0.58f, 1.0f, 0.9f, 0.42f};
    f.n = 6;
    for (int i = 0; i < 6; ++i)
    {
        f.x[(size_t)i] = xs[i];
        f.y[(size_t)i] = ys[i];
    }
    return f;
}

inline void finishPlan(RoomParams &r, const FloorPlan &drawn) noexcept
{
    r.plan = r.box() ? FloorPlan() : makeFloorPlan(r.shape, r.shapeAmt, drawn);
}

/** does a polygon cross itself? (the drawn plan must not) */
inline bool planSelfIntersects(const FloorPlan &f) noexcept
{
    auto cross = [](float ax, float ay, float bx, float by) { return ax * by - ay * bx; };
    for (int i = 0; i < f.n; ++i)
        for (int j = i + 1; j < f.n; ++j)
        {
            if (j == i + 1 || (i == 0 && j == f.n - 1))
                continue; // neighbours share a vertex
            const int i2 = (i + 1) % f.n, j2 = (j + 1) % f.n;
            const float px = f.x[(size_t)i], py = f.y[(size_t)i], rx = f.x[(size_t)i2] - px,
                        ry = f.y[(size_t)i2] - py;
            const float qx = f.x[(size_t)j], qy = f.y[(size_t)j], sx = f.x[(size_t)j2] - qx,
                        sy = f.y[(size_t)j2] - qy;
            const float den = cross(rx, ry, sx, sy);
            if (std::abs(den) < 1.0e-9f)
                continue;
            const float t = cross(qx - px, qy - py, sx, sy) / den,
                        u = cross(qx - px, qy - py, rx, ry) / den;
            if (t > 1.0e-4f && t < 1.0f - 1.0e-4f && u > 1.0e-4f && u < 1.0f - 1.0e-4f)
                return true;
        }
    return false;
}

/** one wall in metres: a -> b, inward unit normal n, signed distance n.p + c (> 0 inside) */
struct Wall2
{
    float ax = 0, ay = 0, bx = 0, by = 0, nx = 0, ny = 0, c = 0, len = 0;
    float dist(float x, float y) const noexcept
    {
        return nx * x + ny * y + c;
    }
};

struct PlanGeo
{
    std::array<Wall2, kMaxPlanVerts> w{};
    int n = 0;
    bool convex = true;
    float area = 0.0f, perimeter = 0.0f;
};

inline PlanGeo planGeo(const RoomParams &r) noexcept
{
    PlanGeo g;
    const float W = r.W(), L = r.L();
    const auto &f = r.plan;
    double area = 0.0;
    for (int i = 0; i < f.n; ++i)
    {
        const int j = (i + 1) % f.n;
        const float ax = f.x[(size_t)i] * W, ay = f.y[(size_t)i] * L, bx = f.x[(size_t)j] * W,
                    by = f.y[(size_t)j] * L;
        area += (double)ax * by - (double)bx * ay;
        const float ex = bx - ax, ey = by - ay, len = std::sqrt(ex * ex + ey * ey);
        if (len < 1.0e-3f)
            continue;
        auto &wl = g.w[(size_t)g.n++];
        wl.ax = ax;
        wl.ay = ay;
        wl.bx = bx;
        wl.by = by;
        wl.len = len;
        wl.nx = -ey / len;
        wl.ny = ex / len; // left of a counter-clockwise edge = inside
        wl.c = -(wl.nx * ax + wl.ny * ay);
        g.perimeter += len;
    }
    g.area = (float)(0.5 * std::abs(area));
    for (int i = 0; i < g.n; ++i)
    {
        const auto &a = g.w[(size_t)i];
        const auto &b = g.w[(size_t)((i + 1) % g.n)];
        const float cr = (a.bx - a.ax) * (b.by - b.ay) - (a.by - a.ay) * (b.bx - b.ax);
        if (cr < -1.0e-6f * a.len * b.len)
            g.convex = false;
    }
    return g;
}

inline bool insidePlan(const PlanGeo &g, float x, float y) noexcept
{
    bool in = false;
    for (int i = 0; i < g.n; ++i)
    {
        const auto &w = g.w[(size_t)i];
        if ((w.ay > y) != (w.by > y))
        {
            const float xc = w.ax + (y - w.ay) / (w.by - w.ay) * (w.bx - w.ax);
            if (x < xc)
                in = !in;
        }
    }
    return in;
}

/** distance to the nearest wall segment, and which */
inline float planEdgeDist(const PlanGeo &g, float x, float y, int *which = nullptr,
                          float *qx = nullptr, float *qy = nullptr) noexcept
{
    float best = 1.0e30f;
    for (int i = 0; i < g.n; ++i)
    {
        const auto &w = g.w[(size_t)i];
        const float ex = w.bx - w.ax, ey = w.by - w.ay;
        const float u =
            juce::jlimit(0.0f, 1.0f, ((x - w.ax) * ex + (y - w.ay) * ey) / (w.len * w.len));
        const float px = w.ax + u * ex, py = w.ay + u * ey;
        const float d = std::sqrt((x - px) * (x - px) + (y - py) * (y - py));
        if (d < best)
        {
            best = d;
            if (which)
                *which = i;
            if (qx)
                *qx = px;
            if (qy)
                *qy = py;
        }
    }
    return best;
}

/** a point moved inside the plan, at least 'margin' from the walls */
inline void keepInsidePlan(const PlanGeo &g, float &x, float &y, float margin) noexcept
{
    if (g.n < 3)
        return;
    int wi = 0;
    float qx = x, qy = y;
    const bool in = insidePlan(g, x, y);
    const float d = planEdgeDist(g, x, y, &wi, &qx, &qy);
    if (in && d >= margin)
        return;
    const auto &w = g.w[(size_t)wi];
    float cx = in ? x + w.nx * (margin - d) : qx + w.nx * margin,
          cy = in ? y + w.ny * (margin - d) : qy + w.ny * margin;
    if (insidePlan(g, cx, cy) && planEdgeDist(g, cx, cy) >= 0.5f * margin)
    {
        x = cx;
        y = cy;
        return;
    }
    // a narrow corner: the inside point farthest from the walls, on a coarse grid
    float bx = x, by = y, bd = -1.0f, x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (int i = 0; i < g.n; ++i)
    {
        x0 = juce::jmin(x0, g.w[(size_t)i].ax);
        x1 = juce::jmax(x1, g.w[(size_t)i].ax);
        y0 = juce::jmin(y0, g.w[(size_t)i].ay);
        y1 = juce::jmax(y1, g.w[(size_t)i].ay);
    }
    for (int iy = 0; iy < 16; ++iy)
        for (int ix = 0; ix < 16; ++ix)
        {
            const float px = x0 + (x1 - x0) * ((float)ix + 0.5f) / 16.0f,
                        py = y0 + (y1 - y0) * ((float)iy + 0.5f) / 16.0f;
            if (!insidePlan(g, px, py))
                continue;
            // nearest to the wanted point among the ones far enough from the walls (else the most
            // open one)
            const float ed = planEdgeDist(g, px, py);
            const float score =
                ed >= margin ? 1.0e6f - std::sqrt((px - x) * (px - x) + (py - y) * (py - y)) : ed;
            if (score > bd)
            {
                bd = score;
                bx = px;
                by = py;
            }
        }
    x = bx;
    y = by;
}

/** a 2D path of wall reflections (walls in order from the source), and its image */
struct PlanPath
{
    int k = 0;
    std::array<uint8_t, 3> w{};
    float ix = 0, iy = 0;
};

/** a mirror image of the polygon room: 'code' identifies the path (walls + floor/ceiling images) */
struct PlanImage
{
    Vec3f pos;
    float dist = 0.0f;
    int code = 0, k = 0, mz = 0, hitsWalls = 0, hitsFloor = 0;
    std::array<uint8_t, 3> w{};
};

inline void reflectOver(const Wall2 &w, float &x, float &y) noexcept
{
    const float d = w.dist(x, y);
    x -= 2.0f * d * w.nx;
    y -= 2.0f * d * w.ny;
}

/** segment p -> q crosses a wall other than the excluded ones (non-convex plans: a wall in the way)
 */
inline bool planBlocked(const PlanGeo &g, float px, float py, float qx, float qy, int ex1,
                        int ex2) noexcept
{
    const float rx = qx - px, ry = qy - py;
    for (int i = 0; i < g.n; ++i)
    {
        if (i == ex1 || i == ex2)
            continue;
        const auto &w = g.w[(size_t)i];
        const float sx = w.bx - w.ax, sy = w.by - w.ay;
        const float den = rx * sy - ry * sx;
        if (std::abs(den) < 1.0e-9f)
            continue;
        const float ox = w.ax - px, oy = w.ay - py;
        const float t = (ox * sy - oy * sx) / den, u = (ox * ry - oy * rx) / den;
        if (t > 1.0e-4f && t < 1.0f - 1.0e-4f && u > 1.0e-4f && u < 1.0f - 1.0e-4f)
            return true;
    }
    return false;
}

/** the reflection points of a path (listener side last), false if the path is not possible.
    S = source, L = listener, images computed from the walls */
inline bool planPathPoints(const PlanGeo &g, const PlanPath &pth, float sx, float sy, float lx,
                           float ly, float *qx, float *qy) noexcept
{
    float ix[4], iy[4];
    ix[0] = sx;
    iy[0] = sy;
    for (int j = 0; j < pth.k; ++j)
    {
        if (pth.w[(size_t)j] >= g.n)
            return false;
        ix[j + 1] = ix[j];
        iy[j + 1] = iy[j];
        reflectOver(g.w[pth.w[(size_t)j]], ix[j + 1], iy[j + 1]);
    }
    float px = lx, py = ly;
    int prev = -1;
    for (int j = pth.k; j >= 1; --j)
    {
        const int wi = pth.w[(size_t)j - 1];
        const auto &w = g.w[(size_t)wi];
        const float dP = w.dist(px, py), dI = w.dist(ix[j], iy[j]);
        if (dP <= 1.0e-4f || dI >= -1.0e-4f)
            return false;
        const float t = dP / (dP - dI);
        const float hx = px + t * (ix[j] - px), hy = py + t * (iy[j] - py);
        const float u =
            ((hx - w.ax) * (w.bx - w.ax) + (hy - w.ay) * (w.by - w.ay)) / (w.len * w.len);
        if (u < -1.0e-4f || u > 1.0f + 1.0e-4f)
            return false;
        if (!g.convex && planBlocked(g, px, py, hx, hy, wi, prev))
            return false;
        qx[j - 1] = hx;
        qy[j - 1] = hy;
        px = hx;
        py = hy;
        prev = wi;
    }
    if (!g.convex && pth.k > 0 && planBlocked(g, px, py, sx, sy, prev, -1))
        return false;
    return true;
}

/** all possible wall-reflection paths up to 'order' (the plan seen from above). Lower orders first.
 */
inline int planPaths(const PlanGeo &g, float sx, float sy, float lx, float ly, int order,
                     PlanPath *out, int maxOut) noexcept
{
    int n = 0;
    float qx[3], qy[3];
    auto tryAdd = [&](const PlanPath &p)
    {
        if (n < maxOut && planPathPoints(g, p, sx, sy, lx, ly, qx, qy))
            out[n++] = p;
    };
    PlanPath p;
    for (int pass = 1; pass <= juce::jlimit(0, 3, order);
         ++pass) // breadth first: the first reflections are never left out
        for (int a = 0; a < g.n; ++a)
        {
            float ax = sx, ay = sy;
            if (g.w[(size_t)a].dist(ax, ay) <= 1.0e-4f)
                continue;
            reflectOver(g.w[(size_t)a], ax, ay);
            if (pass == 1)
            {
                p.k = 1;
                p.w = {(uint8_t)a, 0, 0};
                p.ix = ax;
                p.iy = ay;
                tryAdd(p);
                continue;
            }
            for (int b = 0; b < g.n; ++b)
            {
                if (b == a || g.w[(size_t)b].dist(ax, ay) <= 1.0e-4f)
                    continue; // the image must face the next wall
                float bx = ax, by = ay;
                reflectOver(g.w[(size_t)b], bx, by);
                if (pass == 2)
                {
                    p.k = 2;
                    p.w = {(uint8_t)a, (uint8_t)b, 0};
                    p.ix = bx;
                    p.iy = by;
                    tryAdd(p);
                    continue;
                }
                for (int c = 0; c < g.n; ++c)
                {
                    if (c == b || g.w[(size_t)c].dist(bx, by) <= 1.0e-4f)
                        continue;
                    float cx = bx, cy = by;
                    reflectOver(g.w[(size_t)c], cx, cy);
                    p.k = 3;
                    p.w = {(uint8_t)a, (uint8_t)b, (uint8_t)c};
                    p.ix = cx;
                    p.iy = cy;
                    tryAdd(p);
                }
            }
        }
    return n;
}

inline int planCode(int k, const std::array<uint8_t, 3> &w, int mz) noexcept
{
    return k | (k > 0 ? (int)w[0] << 2 : 0) | (k > 1 ? (int)w[1] << 6 : 0) |
           (k > 2 ? (int)w[2] << 10 : 0) | ((mz + 3) << 14);
}

/** image sources of a polygon room: wall paths combined with floor / ceiling images, at most
   'order' reflections in all. 'paths' and 'cand' are scratch; the nearest 'maxOut' land at the
   front of 'cand'. Returns their number. */
inline int planImageSources(const RoomParams &r, const PlanGeo &g, const Vec3f &src,
                            const Vec3f &lis, int order, PlanPath *paths, int maxPaths,
                            PlanImage *cand, int maxCand, int maxOut) noexcept
{
    order = juce::jlimit(1, 3, order);
    const int np = planPaths(g, src.x, src.y, lis.x, lis.y, order, paths + 1, maxPaths - 1);
    paths[0] = PlanPath(); // the direct path (for floor / ceiling images)
    paths[0].ix = src.x;
    paths[0].iy = src.y;
    const float H = r.H();
    int n = 0;
    for (int i = 0; i <= np && n < maxCand; ++i)
    {
        const auto &p = paths[i];
        const int left = order - p.k;
        for (int mz = -left; mz <= left && n < maxCand; ++mz)
        {
            if (p.k == 0 && mz == 0)
                continue;
            auto &c = cand[n++];
            c.pos = {p.ix, p.iy, imageCoord(mz, H, src.z)};
            const float dx = c.pos.x - lis.x, dy = c.pos.y - lis.y, dz = c.pos.z - lis.z;
            c.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            c.k = p.k;
            c.w = p.w;
            c.mz = mz;
            c.hitsWalls = p.k;
            c.hitsFloor = std::abs(mz);
            c.code = planCode(p.k, p.w, mz);
        }
    }
    if (n > maxOut)
    {
        std::nth_element(cand, cand + maxOut, cand + n,
                         [](const PlanImage &a, const PlanImage &b) { return a.dist < b.dist; });
        n = maxOut;
    }
    return n;
}

/** the image of a known path (no visibility test): for moving sources between two visibility checks
 */
inline bool planImageOf(const PlanGeo &g, int k, const std::array<uint8_t, 3> &w, float sx,
                        float sy, float &ix, float &iy) noexcept
{
    ix = sx;
    iy = sy;
    for (int j = 0; j < k; ++j)
    {
        if (w[(size_t)j] >= g.n)
            return false;
        reflectOver(g.w[w[(size_t)j]], ix, iy);
    }
    return true;
}

inline Vec3f listenerPos(const RoomParams &r) noexcept
{
    if (!r.box())
    {
        const auto g = planGeo(r);
        float x = juce::jlimit(0.0f, 1.0f, r.listenerX) * r.W(),
              y = juce::jlimit(0.0f, 1.0f, r.listenerY) * r.L();
        keepInsidePlan(g, x, y, 0.3f);
        return {x, y, juce::jmin(RoomParams::kEarHeight, r.H() * 0.5f)};
    }
    return {juce::jlimit(0.3f, r.W() - 0.3f, r.listenerX * r.W()),
            juce::jlimit(0.3f, r.L() - 0.3f, r.listenerY * r.L()),
            juce::jmin(RoomParams::kEarHeight, r.H() * 0.5f)};
}

/** a direction / distance seen from the listener -> point in the room (kept 0.3 m inside the walls)
 */
inline Vec3f roomPoint(const RoomParams &r, Pos p) noexcept
{
    const auto L = listenerPos(r);
    const float a = juce::degreesToRadians(p.az), e = juce::degreesToRadians(p.el);
    const float d = p.dist;
    if (!r.box())
    {
        // along the line from the listener, stopped 0.3 m before the first wall (so the direct
        // sound always gets there)
        const auto g = planGeo(r);
        const float hx = -std::sin(a) * std::cos(e), hy = std::cos(a) * std::cos(e);
        const float hl = std::sqrt(hx * hx + hy * hy);
        float reach = d * hl;
        if (hl > 1.0e-6f)
        {
            const float ux = hx / hl, uy = hy / hl;
            for (int i = 0; i < g.n; ++i)
            {
                const auto &w = g.w[(size_t)i];
                const float dn = w.nx * ux + w.ny * uy;
                if (dn > -1.0e-6f)
                    continue; // not moving towards this wall
                const float t = w.dist(L.x, L.y) / -dn;
                const float hx2 = L.x + ux * t, hy2 = L.y + uy * t;
                const float u =
                    ((hx2 - w.ax) * (w.bx - w.ax) + (hy2 - w.ay) * (w.by - w.ay)) / (w.len * w.len);
                if (u < -1.0e-4f || u > 1.0f + 1.0e-4f || t < 0.0f)
                    continue;
                reach = juce::jmin(reach, juce::jmax(0.05f, t - 0.3f / juce::jmax(0.25f, -dn)));
            }
            return {L.x + ux * reach, L.y + uy * reach,
                    juce::jlimit(0.3f, r.H() - 0.3f, L.z + std::sin(e) * d)};
        }
        return {L.x, L.y, juce::jlimit(0.3f, r.H() - 0.3f, L.z + std::sin(e) * d)};
    }
    return {juce::jlimit(0.3f, r.W() - 0.3f, L.x - std::sin(a) * std::cos(e) * d),
            juce::jlimit(0.3f, r.L() - 0.3f, L.y + std::cos(a) * std::cos(e) * d),
            juce::jlimit(0.3f, r.H() - 0.3f, L.z + std::sin(e) * d)};
}

/** a point -> direction / distance from the listener */
inline Pos seenFrom(const Vec3f &L, const Vec3f &q) noexcept
{
    const float dx = q.x - L.x, dy = q.y - L.y, dz = q.z - L.z;
    const float r = std::sqrt(dx * dx + dy * dy + dz * dz);
    Pos p;
    p.az = juce::radiansToDegrees(std::atan2(-dx, dy));
    p.el =
        r > 1.0e-6f ? juce::radiansToDegrees(std::asin(juce::jlimit(-1.0f, 1.0f, dz / r))) : 0.0f;
    p.dist = r;
    return p;
}

/** one mirror image of a source */
struct ImageSource
{
    Vec3f pos;
    int hitsWalls = 0,
        hitsFloor = 0; // reflections on the side/front/back walls, on floor + ceiling
    int mx = 0, my = 0, mz = 0;
};

/** all images up to 'order' reflections (order 1: 6, 2: 24, 3: 62) */
inline int imageSources(const RoomParams &r, const Vec3f &src, int order, ImageSource *out,
                        int maxOut) noexcept
{
    int n = 0;
    order = juce::jlimit(1, 3, order);
    for (int mx = -order; mx <= order; ++mx)
        for (int my = -order; my <= order; ++my)
            for (int mz = -order; mz <= order; ++mz)
            {
                const int k = std::abs(mx) + std::abs(my) + std::abs(mz);
                if (k == 0 || k > order || n >= maxOut)
                    continue;
                ImageSource &im = out[n++];
                im.pos = {imageCoord(mx, r.W(), src.x), imageCoord(my, r.L(), src.y),
                          imageCoord(mz, r.H(), src.z)};
                im.hitsWalls = std::abs(mx) + std::abs(my);
                im.hitsFloor = std::abs(mz);
                im.mx = mx;
                im.my = my;
                im.mz = mz;
            }
    return n;
}

/** Eyring decay time (s) of the room in one band */
inline float eyringRt(const RoomParams &r, bool high, float air) noexcept
{
    const auto a = BandAbsorption::from(r);
    const float W = r.W(), L = r.L(), H = r.H();
    float V = W * L * H;
    float Sw = 2.0f * (W * H + L * H), Sf = 2.0f * W * L;
    if (!r.box())
    {
        const auto g = planGeo(r);
        V = g.area * H;
        Sw = g.perimeter * H;
        Sf = 2.0f * g.area;
    }
    const float S = Sw + Sf;
    const float am =
        ((high ? a.wallHigh : a.wallLow) * Sw + (high ? a.floorHigh : a.floorLow) * Sf) / S;
    const float m = high ? airHigh(air) : airLow();
    const float denom = -S * std::log(juce::jmax(1.0e-4f, 1.0f - am)) + 4.0f * m * V;
    return juce::jlimit(0.05f, 30.0f, 0.161f * V / juce::jmax(1.0e-4f, denom));
}

inline float roomVolume(const RoomParams &r) noexcept
{
    return r.box() ? r.W() * r.L() * r.H() : planGeo(r).area * r.H();
}
inline float meanFreePath(const RoomParams &r) noexcept
{
    if (!r.box())
    {
        const auto g = planGeo(r);
        return 4.0f * g.area * r.H() / juce::jmax(1.0e-3f, 2.0f * g.area + g.perimeter * r.H());
    }
    const float S = 2.0f * (r.W() * r.L() + r.W() * r.H() + r.L() * r.H());
    return 4.0f * roomVolume(r) / S;
}

/** Ray tracing (energy, two bands): rays leave a source, reflect specularly or (with probability
   'scatter') diffusely (Lambert), lose energy at every wall and in the air. The energy left in the
   whole ray ensemble over time is the room's decay; its slope between -5 and -35 dB gives T30. This
   catches what Eyring cannot: uneven absorption (e.g. a dead floor and hard walls), long or flat
   rooms where the sound field is not diffuse, and how scattering mixes the energy. */
struct RayResult
{
    float rtLow = 1.0f, rtHigh = 1.0f; // s
    int rays = 0, reflections = 0;
    bool valid = false;
};

inline RayResult traceRoom(const RoomParams &r, float air, int numRays = 3000,
                           uint32_t seed = 12345)
{
    RayResult res;
    const auto ab = BandAbsorption::from(r);
    const float W = r.W(), L = r.L(), H = r.H();
    constexpr float binSec = 0.005f, maxSec = 30.0f;
    const int bins = (int)(maxSec / binSec);
    std::vector<double> eLow((size_t)bins, 0.0), eHigh((size_t)bins, 0.0);
    XorShift rng(seed);
    const float mL = airLow(), mH = airHigh(air);
    Vec3f src{W * 0.5f, L * 0.75f, juce::jmin(H * 0.5f, 4.0f)}; // roughly where an organ stands
    const bool plan = !r.box();
    PlanGeo g;
    if (plan)
    {
        g = planGeo(r);
        keepInsidePlan(g, src.x, src.y, 0.3f);
    }
    int totalRefl = 0;
    for (int ray = 0; ray < numRays; ++ray)
    {
        // uniform direction
        const float z = rng.bi(), phi = juce::MathConstants<float>::twoPi * rng.uni();
        const float s = std::sqrt(juce::jmax(0.0f, 1.0f - z * z));
        Vec3f d{s * std::cos(phi), s * std::sin(phi), z};
        Vec3f p = src;
        double el = 1.0, eh = 1.0, t = 0.0;
        for (int hit = 0; hit < 4000 && t < maxSec && (el > 1.0e-7 || eh > 1.0e-7); ++hit)
        {
            // distance to the next wall
            float tx = 1e30f, ty = 1e30f, tz = 1e30f;
            if (d.x > 1e-9f)
                tx = (W - p.x) / d.x;
            else if (d.x < -1e-9f)
                tx = -p.x / d.x;
            if (d.y > 1e-9f)
                ty = (L - p.y) / d.y;
            else if (d.y < -1e-9f)
                ty = -p.y / d.y;
            if (d.z > 1e-9f)
                tz = (H - p.z) / d.z;
            else if (d.z < -1e-9f)
                tz = -p.z / d.z;
            int wallHit = -1;
            if (plan)
            {
                // the walls of the plan instead of the x / y planes
                tx = ty = 1e30f;
                for (int i = 0; i < g.n; ++i)
                {
                    const auto &w = g.w[(size_t)i];
                    const float dn = w.nx * d.x + w.ny * d.y;
                    if (dn > -1.0e-9f)
                        continue;
                    const float tw = juce::jmax(0.0f, w.dist(p.x, p.y)) / -dn;
                    if (tw >= tx)
                        continue;
                    const float hx = p.x + d.x * tw, hy = p.y + d.y * tw;
                    const float u = ((hx - w.ax) * (w.bx - w.ax) + (hy - w.ay) * (w.by - w.ay)) /
                                    (w.len * w.len);
                    if (u < -1.0e-3f || u > 1.0f + 1.0e-3f)
                        continue;
                    tx = tw;
                    wallHit = i;
                }
                if (wallHit < 0 && tz > 1e29f)
                    break; // lost (numerics): this ray ends
            }
            const float dist = juce::jmax(1.0e-4f, juce::jmin(tx, ty, tz));
            const int axis = tx <= ty && tx <= tz ? 0 : (ty <= tz ? 1 : 2);
            // energy along the segment (air absorption inside the segment is folded in at its end)
            const double t1 = t + dist / kSpeedOfSoundRoom;
            const int b0 = (int)(t / binSec), b1 = juce::jmin(bins - 1, (int)(t1 / binSec));
            for (int b = b0; b <= b1 && b < bins; ++b)
            {
                const double lo = juce::jmax(t, (double)b * binSec),
                             hi = juce::jmin(t1, (double)(b + 1) * binSec);
                if (hi > lo)
                {
                    eLow[(size_t)b] += el * (hi - lo);
                    eHigh[(size_t)b] += eh * (hi - lo);
                }
            }
            t = t1;
            p = {p.x + d.x * dist, p.y + d.y * dist, p.z + d.z * dist};
            el *= std::exp(-mL * dist);
            eh *= std::exp(-mH * dist);
            const bool floor = axis == 2;
            el *= 1.0 - (floor ? ab.floorLow : ab.wallLow);
            eh *= 1.0 - (floor ? ab.floorHigh : ab.wallHigh);
            ++totalRefl;
            if (plan && axis == 0)
            {
                // a wall of the plan: reflect about its normal (or scatter around it)
                const auto &w = g.w[(size_t)wallHit];
                if (rng.uni() < r.scatter)
                {
                    const float u1 = rng.uni(), u2 = rng.uni();
                    const float rr = std::sqrt(u1), th = juce::MathConstants<float>::twoPi * u2;
                    const float a = rr * std::cos(th), b = rr * std::sin(th),
                                c = std::sqrt(juce::jmax(0.0f, 1.0f - u1));
                    d = {w.nx * c - w.ny * a, w.ny * c + w.nx * a, b};
                }
                else
                {
                    const float dn = d.x * w.nx + d.y * w.ny;
                    d.x -= 2.0f * dn * w.nx;
                    d.y -= 2.0f * dn * w.ny;
                }
                p.x += w.nx * 1.0e-4f;
                p.y += w.ny * 1.0e-4f;
                p.z = juce::jlimit(1.0e-4f, H - 1.0e-4f, p.z);
                continue;
            }
            // new direction: specular, or Lambert around the wall normal
            Vec3f nrm{0, 0, 0};
            if (axis == 0)
                nrm.x = d.x > 0 ? -1.0f : 1.0f;
            else if (axis == 1)
                nrm.y = d.y > 0 ? -1.0f : 1.0f;
            else
                nrm.z = d.z > 0 ? -1.0f : 1.0f;
            if (rng.uni() < r.scatter)
            {
                // cosine-weighted hemisphere
                const float u1 = rng.uni(), u2 = rng.uni();
                const float rr = std::sqrt(u1), th = juce::MathConstants<float>::twoPi * u2;
                const float a = rr * std::cos(th), b = rr * std::sin(th),
                            c = std::sqrt(juce::jmax(0.0f, 1.0f - u1));
                if (axis == 0)
                    d = {nrm.x * c, a, b};
                else if (axis == 1)
                    d = {a, nrm.y * c, b};
                else
                    d = {a, b, nrm.z * c};
            }
            else
            {
                if (axis == 0)
                    d.x = -d.x;
                else if (axis == 1)
                    d.y = -d.y;
                else
                    d.z = -d.z;
            }
            // keep the point inside
            if (!plan)
            {
                p.x = juce::jlimit(1.0e-4f, W - 1.0e-4f, p.x);
                p.y = juce::jlimit(1.0e-4f, L - 1.0e-4f, p.y);
            }
            p.z = juce::jlimit(1.0e-4f, H - 1.0e-4f, p.z);
        }
    }
    // decay time from the ensemble energy: least-squares line through -5 .. -35 dB of the
    // backward-integrated curve
    auto fit = [&](const std::vector<double> &e)
    {
        std::vector<double> sch((size_t)bins);
        double acc = 0;
        for (int b = bins - 1; b >= 0; --b)
        {
            acc += e[(size_t)b];
            sch[(size_t)b] = acc;
        }
        if (acc <= 0)
            return 30.0f;
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int n = 0;
        for (int b = 0; b < bins; ++b)
        {
            const double db = 10.0 * std::log10(juce::jmax(1e-30, sch[(size_t)b] / acc));
            if (db > -5.0)
                continue;
            if (db < -35.0)
                break;
            const double x = static_cast<double>(b) * binSec;
            sx += x;
            sy += db;
            sxx += x * x;
            sxy += x * db;
            ++n;
        }
        if (n < 3)
            return 30.0f;
        const double slope =
            (n * sxy - sx * sy) / juce::jmax(1e-12, n * sxx - sx * sx); // dB per second
        return slope < -1e-3 ? juce::jlimit(0.05f, 30.0f, (float)(-60.0 / slope)) : 30.0f;
    };
    res.rtLow = fit(eLow);
    res.rtHigh = fit(eHigh);
    res.rays = numRays;
    res.reflections = totalRefl;
    res.valid = true;
    return res;
}
} // namespace rysum
