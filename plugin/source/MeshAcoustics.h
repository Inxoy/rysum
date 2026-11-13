#pragma once

#include "RoomSim.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace rysum
{
/** A loaded room model (OBJ) as the sound sees it: its triangles in a tree of boxes, so a ray finds
    what it hits in a few steps. Used for
      - the late reverb: rays traced through the model give the decay (traceMesh),
      - line of sight: a pipe behind a wall or a pillar of the model is heard around it (meshDetour),
      - walking: the model's walls stop you, its floors carry you (the 3D view).
    The Saal's shoebox stays underneath: it gives the early reflections, closes a model that is open
    somewhere (no ray is lost), and keeps listener and pipes inside.
    Built once on the message thread; casting allocates nothing (safe on the audio thread). */
class MeshAcoustics
{
public:
    enum Kind : uint8_t
    {
        wall = 0,
        floor,
        ceiling
    };
    struct Tri
    {
        Vec3f a, e1, e2, n; // corner, two edges, unit normal
        uint8_t kind = wall;
    };

    std::vector<Tri> tris;
    Vec3f size; // bounding box (m), the model's corner at 0 / 0 / 0
    uint32_t serial = 0;

    /** 'verts': 9 floats per triangle (a, b, c) in room coordinates */
    static std::shared_ptr<MeshAcoustics> build(const float *verts, int numTris, Vec3f size,
                                                uint32_t serial)
    {
        auto m = std::make_shared<MeshAcoustics>();
        m->size = size;
        m->serial = serial;
        m->tris.reserve((size_t)numTris);
        std::vector<Vec3f> cen;
        cen.reserve((size_t)numTris);
        for (int i = 0; i < numTris; ++i)
        {
            const float *v = verts + 9 * i;
            Tri t;
            t.a = {v[0], v[1], v[2]};
            t.e1 = {v[3] - v[0], v[4] - v[1], v[5] - v[2]};
            t.e2 = {v[6] - v[0], v[7] - v[1], v[8] - v[2]};
            Vec3f n{t.e1.y * t.e2.z - t.e1.z * t.e2.y, t.e1.z * t.e2.x - t.e1.x * t.e2.z,
                    t.e1.x * t.e2.y - t.e1.y * t.e2.x};
            const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (l < 1.0e-10f)
                continue; // no area
            t.n = {n.x / l, n.y / l, n.z / l};
            const float zc = (v[2] + v[5] + v[8]) / 3.0f;
            // level faces low down are floor, level faces high up ceiling, the rest walls
            t.kind = std::abs(t.n.z) < 0.8f ? wall : (zc < 0.5f * size.z ? floor : ceiling);
            m->tris.push_back(t);
            cen.push_back({(v[0] + v[3] + v[6]) / 3.0f, (v[1] + v[4] + v[7]) / 3.0f, zc});
        }
        if (m->tris.empty())
            return nullptr;
        std::vector<int> idx(m->tris.size());
        for (size_t i = 0; i < idx.size(); ++i)
            idx[i] = (int)i;
        m->nodes.reserve(2 * m->tris.size() / 3 + 8);
        m->buildNode(idx, cen, 0, (int)idx.size(), 0);
        // the triangles in the order of the leaves (a leaf = a run of them)
        std::vector<Tri> sorted(m->tris.size());
        for (size_t i = 0; i < idx.size(); ++i)
            sorted[i] = m->tris[(size_t)idx[i]];
        m->tris.swap(sorted);
        return m;
    }

    /** the nearest triangle the ray o + d t meets for tMin < t < tMax (d need not be a unit
        vector); its index or -1 */
    int cast(Vec3f o, Vec3f d, float tMax, float &tHit, float tMin = 1.0e-4f) const noexcept
    {
        return traverse(o, d, tMin, tMax, tHit, false);
    }

    /** is anything between a and b? (the ends themselves do not count) */
    bool blocked(Vec3f a, Vec3f b) const noexcept
    {
        float t;
        return traverse(a, {b.x - a.x, b.y - a.y, b.z - a.z}, 1.0e-3f, 1.0f - 1.0e-3f, t, true) >=
               0;
    }

    /** how many surfaces lie between a and b (a wall has two) - at most 'maxCount' */
    int crossings(Vec3f a, Vec3f b, int maxCount = 8) const noexcept
    {
        const Vec3f d{b.x - a.x, b.y - a.y, b.z - a.z};
        float from = 1.0e-3f;
        int n = 0;
        while (n < maxCount)
        {
            float t;
            if (traverse(a, d, from, 1.0f - 1.0e-3f, t, false) < 0)
                break;
            ++n;
            from = t + 1.0e-3f;
        }
        return n;
    }

private:
    struct Node
    {
        Vec3f lo, hi;
        int left = -1, right = -1; // children (inner node)
        int first = 0, count = 0;  // triangles (leaf: count > 0)
    };
    std::vector<Node> nodes;

    int buildNode(std::vector<int> &idx, const std::vector<Vec3f> &cen, int a, int b, int depth)
    {
        const int me = (int)nodes.size();
        nodes.emplace_back();
        Vec3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f}, clo = lo, chi = hi;
        for (int i = a; i < b; ++i)
        {
            const auto &t = tris[(size_t)idx[(size_t)i]];
            const Vec3f p[3]{t.a,
                             {t.a.x + t.e1.x, t.a.y + t.e1.y, t.a.z + t.e1.z},
                             {t.a.x + t.e2.x, t.a.y + t.e2.y, t.a.z + t.e2.z}};
            for (const auto &q : p)
            {
                lo = {juce::jmin(lo.x, q.x), juce::jmin(lo.y, q.y), juce::jmin(lo.z, q.z)};
                hi = {juce::jmax(hi.x, q.x), juce::jmax(hi.y, q.y), juce::jmax(hi.z, q.z)};
            }
            const auto &c = cen[(size_t)idx[(size_t)i]];
            clo = {juce::jmin(clo.x, c.x), juce::jmin(clo.y, c.y), juce::jmin(clo.z, c.z)};
            chi = {juce::jmax(chi.x, c.x), juce::jmax(chi.y, c.y), juce::jmax(chi.z, c.z)};
        }
        nodes[(size_t)me].lo = lo;
        nodes[(size_t)me].hi = hi;
        const float ex = chi.x - clo.x, ey = chi.y - clo.y, ez = chi.z - clo.z;
        if (b - a <= 4 || depth > 40 || juce::jmax(ex, ey, ez) < 1.0e-5f)
        {
            nodes[(size_t)me].first = a;
            nodes[(size_t)me].count = b - a;
            return me;
        }
        const int axis = ex >= ey && ex >= ez ? 0 : (ey >= ez ? 1 : 2);
        auto key = [&](int i)
        {
            const auto &c = cen[(size_t)i];
            return axis == 0 ? c.x : (axis == 1 ? c.y : c.z);
        };
        const int mid = (a + b) / 2;
        std::nth_element(idx.begin() + a, idx.begin() + mid, idx.begin() + b,
                         [&](int p, int q) { return key(p) < key(q); });
        const int l = buildNode(idx, cen, a, mid, depth + 1);
        const int r = buildNode(idx, cen, mid, b, depth + 1);
        nodes[(size_t)me].left = l;
        nodes[(size_t)me].right = r;
        return me;
    }

    static bool hitsBox(const Node &n, Vec3f o, Vec3f inv, float t0, float t1) noexcept
    {
        float a = (n.lo.x - o.x) * inv.x, b = (n.hi.x - o.x) * inv.x;
        t0 = juce::jmax(t0, juce::jmin(a, b));
        t1 = juce::jmin(t1, juce::jmax(a, b));
        a = (n.lo.y - o.y) * inv.y;
        b = (n.hi.y - o.y) * inv.y;
        t0 = juce::jmax(t0, juce::jmin(a, b));
        t1 = juce::jmin(t1, juce::jmax(a, b));
        a = (n.lo.z - o.z) * inv.z;
        b = (n.hi.z - o.z) * inv.z;
        t0 = juce::jmax(t0, juce::jmin(a, b));
        t1 = juce::jmin(t1, juce::jmax(a, b));
        return t0 <= t1;
    }

    int traverse(Vec3f o, Vec3f d, float tMin, float tMax, float &tHit, bool any) const noexcept
    {
        if (nodes.empty())
            return -1;
        auto safeInv = [](float v)
        { return 1.0f / (std::abs(v) > 1.0e-12f ? v : (v < 0.0f ? -1.0e-12f : 1.0e-12f)); };
        const Vec3f inv{safeInv(d.x), safeInv(d.y), safeInv(d.z)};
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        int best = -1;
        float bestT = tMax;
        while (sp > 0)
        {
            const Node &n = nodes[(size_t)stack[--sp]];
            if (!hitsBox(n, o, inv, tMin, bestT))
                continue;
            if (n.count > 0)
            {
                for (int i = n.first; i < n.first + n.count; ++i)
                {
                    // Moeller-Trumbore
                    const Tri &t = tris[(size_t)i];
                    const Vec3f p{d.y * t.e2.z - d.z * t.e2.y, d.z * t.e2.x - d.x * t.e2.z,
                                  d.x * t.e2.y - d.y * t.e2.x};
                    const float det = t.e1.x * p.x + t.e1.y * p.y + t.e1.z * p.z;
                    if (std::abs(det) < 1.0e-12f)
                        continue;
                    const float id = 1.0f / det;
                    const Vec3f s{o.x - t.a.x, o.y - t.a.y, o.z - t.a.z};
                    const float u = (s.x * p.x + s.y * p.y + s.z * p.z) * id;
                    if (u < 0.0f || u > 1.0f)
                        continue;
                    const Vec3f q{s.y * t.e1.z - s.z * t.e1.y, s.z * t.e1.x - s.x * t.e1.z,
                                  s.x * t.e1.y - s.y * t.e1.x};
                    const float v = (d.x * q.x + d.y * q.y + d.z * q.z) * id;
                    if (v < 0.0f || u + v > 1.0f)
                        continue;
                    const float tt = (t.e2.x * q.x + t.e2.y * q.y + t.e2.z * q.z) * id;
                    if (tt > tMin && tt < bestT)
                    {
                        bestT = tt;
                        best = i;
                        if (any)
                        {
                            tHit = tt;
                            return i;
                        }
                    }
                }
            }
            else if (sp < 62)
            {
                stack[sp++] = n.left;
                stack[sp++] = n.right;
            }
        }
        tHit = bestT;
        return best;
    }
};

// ---------------------------------------------------------------------------
// line of sight in a model
// ---------------------------------------------------------------------------
/** The way from listener L to source S when the model is in between: around (or over) the nearest
    edge that lets the sound through with one bend. Returns false if the line of sight is free.
    'out' = where it is heard from (the direction of the bend, the whole way's length); lossDb /
    cutoffHz grow with the detour - as in the floor plans (diffractionLoss), so nothing jumps at
    the shadow's edge. No way with one bend: through the walls, each one costing
    level and highs. */
inline bool meshDetour(const MeshAcoustics &m, Vec3f L, Vec3f S, Pos &out, float &lossDb,
                       float &cutoffHz) noexcept
{
    if (!m.blocked(L, S))
        return false;
    const Vec3f d{S.x - L.x, S.y - L.y, S.z - L.z};
    const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (dist < 1.0e-3f)
        return false;
    const Vec3f u0{d.x / dist, d.y / dist, d.z / dist};
    // two directions across the line of sight
    Vec3f a = std::abs(u0.z) < 0.9f ? Vec3f{-u0.y, u0.x, 0.0f} : Vec3f{1.0f, 0.0f, 0.0f};
    {
        const float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
        a = {a.x / l, a.y / l, a.z / l};
    }
    const Vec3f b{u0.y * a.z - u0.z * a.y, u0.z * a.x - u0.x * a.z, u0.x * a.y - u0.y * a.x};
    // the point on the ray L + u t nearest to S (kept short of whatever the ray hits), if it sees S
    auto waypoint = [&](Vec3f u, Vec3f &W) -> bool
    {
        float t = u.x * d.x + u.y * d.y + u.z * d.z;
        if (t <= 0.05f)
            return false;
        float hit;
        if (m.cast(L, u, t + 0.05f, hit) >= 0)
        {
            if (hit < 0.3f)
                return false;
            t = hit - 0.1f; // the ray ends on a wall: stop a little before it
        }
        // along the ray, the first point that sees S: just past the edge of what is in the way
        // (further on the way only gets longer and the bend sharper)
        constexpr int kSteps = 5;
        float prev = 0.0f;
        for (int i = 1; i <= kSteps; ++i)
        {
            const float ti = t * (float)i / (float)kSteps;
            Vec3f P{L.x + u.x * ti, L.y + u.y * ti, L.z + u.z * ti};
            if (m.blocked(P, S))
            {
                prev = ti;
                continue;
            }
            // (twice halved towards the last point that did not: close to the edge)
            float lo = prev, hi = ti;
            for (int k = 0; k < 2; ++k)
            {
                const float mid = 0.5f * (lo + hi);
                const Vec3f Q{L.x + u.x * mid, L.y + u.y * mid, L.z + u.z * mid};
                if (mid > 0.05f && !m.blocked(Q, S))
                {
                    hi = mid;
                    P = Q;
                }
                else
                    lo = mid;
            }
            W = P;
            return true;
        }
        return false;
    };
    auto dirAt = [&](float ang, float phi)
    {
        const float ca = std::cos(ang), sa = std::sin(ang), cp = std::cos(phi), sp = std::sin(phi);
        return Vec3f{u0.x * ca + (a.x * cp + b.x * sp) * sa, u0.y * ca + (a.y * cp + b.y * sp) * sa,
                     u0.z * ca + (a.z * cp + b.z * sp) * sa};
    };
    auto len = [](Vec3f p, Vec3f q)
    {
        return std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) +
                         (q.z - p.z) * (q.z - p.z));
    };
    // rings of growing angle around the line of sight: the first ring with a way through decides
    // (the smaller the bend, the shorter the way and the louder the sound)
    constexpr int kRings = 12, kPerRing = 12;
    float best = 1.0e30f, bestAng = 0.0f, bestPhi = 0.0f;
    Vec3f bestW{};
    for (int ring = 1; ring <= kRings && best > 1.0e29f; ++ring)
    {
        const float ang = juce::degreesToRadians(7.0f * (float)ring);
        for (int k = 0; k < kPerRing; ++k)
        {
            const float phi = juce::MathConstants<float>::twoPi * ((float)k + 0.5f * (float)(ring & 1)) /
                              (float)kPerRing;
            Vec3f W;
            if (!waypoint(dirAt(ang, phi), W))
                continue;
            const float way = len(L, W) + len(W, S);
            if (way < best)
            {
                best = way;
                bestW = W;
                bestAng = ang;
                bestPhi = phi;
            }
        }
    }
    if (best > 1.0e29f)
    {
        // no way around: through the walls (each surface in between takes level and highs)
        const int n = m.crossings(L, S, 6);
        out = seenFrom(L, S);
        lossDb = juce::jmin(36.0f, 9.0f * (float)juce::jmax(1, n));
        cutoffHz = juce::jmax(250.0f, 1600.0f / (float)juce::jmax(1, n));
        return true;
    }
    // towards the line of sight until the way closes: the bend as small as the edge allows
    {
        float lo = 0.0f, hi = bestAng;
        for (int i = 0; i < 5; ++i)
        {
            const float mid = 0.5f * (lo + hi);
            Vec3f W;
            if (waypoint(dirAt(mid, bestPhi), W))
            {
                hi = mid;
                bestW = W;
                best = len(L, W) + len(W, S);
            }
            else
                lo = mid;
        }
    }
    const Vec3f w1{bestW.x - L.x, bestW.y - L.y, bestW.z - L.z},
        w2{S.x - bestW.x, S.y - bestW.y, S.z - bestW.z};
    const float l1 = len(L, bestW), l2 = len(bestW, S);
    float bend = 0.0f;
    if (l1 > 1.0e-4f && l2 > 1.0e-4f)
        bend = std::acos(
            juce::jlimit(-1.0f, 1.0f, (w1.x * w2.x + w1.y * w2.y + w1.z * w2.z) / (l1 * l2)));
    out = seenFrom(L, bestW);
    out.dist = juce::jmax(dist, best);
    const float bn = juce::jlimit(0.0f, 1.0f, bend / juce::MathConstants<float>::pi);
    diffractionLoss(best - dist, bn, lossDb, cutoffHz);
    return true;
}

// ---------------------------------------------------------------------------
// the decay of a model
// ---------------------------------------------------------------------------
/** traceRoom for a loaded model: the rays bounce off the model's triangles (its floors with the
    absorption of the Saal's floor, its ceilings with the ceiling's, everything else with the mean
    of the walls), specular or scattered as in the Saal. The Saal's shoebox is underneath: a ray
    that finds no triangle (a model open at the top, a doorway) meets the Saal's wall there, so
    none is lost and an open model still has the Saal's size. */
inline RayResult traceMesh(const MeshAcoustics &mesh, const RoomParams &r, float air,
                           int numRays = 1500, uint32_t seed = 12345)
{
    RayResult res;
    const auto ab = SurfAbs::from(r);
    float wallLo, wallHi;
    ab.meanWall(r.box() ? 4 : r.plan.n, wallLo, wallHi);
    // the box that closes the model: the Saal, grown to hold all of the model
    const float W = juce::jmax(r.W(), mesh.size.x), L = juce::jmax(r.L(), mesh.size.y),
                H = juce::jmax(r.H(), mesh.size.z);
    constexpr float binSec = 0.005f, maxSec = 30.0f;
    const int bins = (int)(maxSec / binSec);
    std::vector<double> eLow((size_t)bins, 0.0), eHigh((size_t)bins, 0.0);
    XorShift rng(seed);
    const float mL = airLow(), mH = airHigh(air);
    const Vec3f src{juce::jmin(W, mesh.size.x) * 0.5f, juce::jmin(L, mesh.size.y) * 0.75f,
                    juce::jmin(H * 0.5f, 4.0f)};
    int totalRefl = 0;
    for (int ray = 0; ray < numRays; ++ray)
    {
        const float z = rng.bi(), phi = juce::MathConstants<float>::twoPi * rng.uni();
        const float s = std::sqrt(juce::jmax(0.0f, 1.0f - z * z));
        Vec3f d{s * std::cos(phi), s * std::sin(phi), z};
        Vec3f p = src;
        double el = 1.0, eh = 1.0, t = 0.0;
        for (int hit = 0; hit < 1500 && t < maxSec && (el > 1.0e-6 || eh > 1.0e-6); ++hit)
        {
            // the shoebox underneath
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
            float dist = juce::jmax(1.0e-4f, juce::jmin(tx, ty, tz));
            const int axis = tx <= ty && tx <= tz ? 0 : (ty <= tz ? 1 : 2);
            Vec3f n{0, 0, 0};
            float aLo, aHi;
            if (axis == 2)
            {
                n.z = d.z > 0 ? -1.0f : 1.0f;
                const int sf = d.z > 0 ? kSurfCeiling : kSurfFloor;
                aLo = ab.low[(size_t)sf];
                aHi = ab.high[(size_t)sf];
            }
            else
            {
                if (axis == 0)
                    n.x = d.x > 0 ? -1.0f : 1.0f;
                else
                    n.y = d.y > 0 ? -1.0f : 1.0f;
                aLo = wallLo;
                aHi = wallHi;
            }
            // the model, if it is nearer
            float tm;
            const int ti = mesh.cast(p, d, dist, tm, 2.0e-4f);
            if (ti >= 0)
            {
                const auto &tr = mesh.tris[(size_t)ti];
                dist = juce::jmax(1.0e-4f, tm);
                n = tr.n;
                if (n.x * d.x + n.y * d.y + n.z * d.z > 0.0f)
                    n = {-n.x, -n.y, -n.z};
                const int sf = tr.kind == MeshAcoustics::floor
                                   ? kSurfFloor
                                   : (tr.kind == MeshAcoustics::ceiling ? kSurfCeiling : -1);
                aLo = sf >= 0 ? ab.low[(size_t)sf] : wallLo;
                aHi = sf >= 0 ? ab.high[(size_t)sf] : wallHi;
            }
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
            el *= std::exp(-mL * dist) * (1.0 - aLo);
            eh *= std::exp(-mH * dist) * (1.0 - aHi);
            ++totalRefl;
            if (rng.uni() < r.scatter)
            {
                // cosine-weighted around the normal
                const float u1 = rng.uni(), u2 = rng.uni();
                const float rr = std::sqrt(u1), th = juce::MathConstants<float>::twoPi * u2;
                const float ca = rr * std::cos(th), cb = rr * std::sin(th),
                            cn = std::sqrt(juce::jmax(0.0f, 1.0f - u1));
                Vec3f ta = std::abs(n.z) < 0.9f ? Vec3f{-n.y, n.x, 0.0f} : Vec3f{1.0f, 0.0f, 0.0f};
                const float tl = std::sqrt(ta.x * ta.x + ta.y * ta.y + ta.z * ta.z);
                ta = {ta.x / tl, ta.y / tl, ta.z / tl};
                const Vec3f tb{n.y * ta.z - n.z * ta.y, n.z * ta.x - n.x * ta.z,
                               n.x * ta.y - n.y * ta.x};
                d = {n.x * cn + ta.x * ca + tb.x * cb, n.y * cn + ta.y * ca + tb.y * cb,
                     n.z * cn + ta.z * ca + tb.z * cb};
            }
            else
            {
                const float dn = d.x * n.x + d.y * n.y + d.z * n.z;
                d = {d.x - 2.0f * dn * n.x, d.y - 2.0f * dn * n.y, d.z - 2.0f * dn * n.z};
            }
            // a hair off the surface, and inside the box
            p = {juce::jlimit(1.0e-4f, W - 1.0e-4f, p.x + n.x * 1.0e-3f),
                 juce::jlimit(1.0e-4f, L - 1.0e-4f, p.y + n.y * 1.0e-3f),
                 juce::jlimit(1.0e-4f, H - 1.0e-4f, p.z + n.z * 1.0e-3f)};
        }
    }
    res.rtLow = fitDecay(eLow, binSec);
    res.rtHigh = fitDecay(eHigh, binSec);
    res.rays = numRays;
    res.reflections = totalRefl;
    res.valid = true;
    return res;
}
} // namespace rysum
