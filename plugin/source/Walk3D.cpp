#include "Walk3D.h"
#include <cmath>
#include <cstdlib>
#include <map>

namespace rysum
{
// ---------------------------------------------------------------------------
// OBJ import
// ---------------------------------------------------------------------------
namespace
{
constexpr int kMaxTris = 80000;

const char* skipSpace (const char* p, const char* end) noexcept { while (p < end && (*p == ' ' || *p == '\t')) ++p; return p; }
const char* skipWord (const char* p, const char* end) noexcept { while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p; return p; }

std::map<juce::String, juce::Colour> readMtl (const juce::File& f)
{
    std::map<juce::String, juce::Colour> out;
    if (! f.existsAsFile()) return out;
    juce::StringArray lines;
    f.readLines (lines);
    juce::String current;
    for (auto l : lines)
    {
        l = l.trim();
        if (l.startsWith ("newmtl")) current = l.fromFirstOccurrenceOf (" ", false, false).trim();
        else if (l.startsWith ("Kd ") && current.isNotEmpty())
        {
            auto t = juce::StringArray::fromTokens (l.substring (3), " \t", "");
            t.removeEmptyStrings();
            if (t.size() >= 3)
                out[current] = juce::Colour::fromFloatRGBA (juce::jlimit (0.0f, 1.0f, t[0].getFloatValue()),
                                                            juce::jlimit (0.0f, 1.0f, t[1].getFloatValue()),
                                                            juce::jlimit (0.0f, 1.0f, t[2].getFloatValue()), 1.0f);
        }
    }
    return out;
}
} // namespace

std::shared_ptr<RoomMesh> loadObjRoom (const juce::File& file, bool zUp, float scale, juce::String& error)
{
    juce::MemoryBlock mb;
    if (! file.loadFileAsData (mb) || mb.getSize() == 0) { error = juce::String::fromUTF8 ("Datei nicht lesbar: ") + file.getFileName(); return nullptr; }
    const char* p = static_cast<const char*> (mb.getData());
    const char* const end = p + mb.getSize();

    std::vector<V3> verts;
    verts.reserve (4096);
    auto mesh = std::make_shared<RoomMesh>();
    mesh->name = file.getFileName();
    std::map<juce::String, juce::Colour> mtl;
    const juce::Colour stone (0xffbdb6a8);
    juce::Colour colour = stone;
    std::vector<int> face;
    scale = juce::jlimit (1.0e-4f, 1.0e4f, scale);

    while (p < end)
    {
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n') ++lineEnd;
        const char* q = skipSpace (p, lineEnd);
        if (q + 1 < lineEnd && q[0] == 'v' && (q[1] == ' ' || q[1] == '\t'))
        {
            char* e = nullptr;
            const float x = std::strtof (q + 2, &e);
            const float y = std::strtof (e, &e);
            const float z = std::strtof (e, &e);
            verts.push_back (zUp ? V3 { x * scale, y * scale, z * scale } : V3 { x * scale, -z * scale, y * scale });
        }
        else if (q + 1 < lineEnd && q[0] == 'f' && (q[1] == ' ' || q[1] == '\t'))
        {
            face.clear();
            const char* r = q + 2;
            while (true)
            {
                r = skipSpace (r, lineEnd);
                if (r >= lineEnd || *r == '\r') break;
                char* e = nullptr;
                const long idx = std::strtol (r, &e, 10);
                if (e == r) break;
                const long n = (long) verts.size();
                const long i = idx > 0 ? idx - 1 : n + idx;          // negative = counted from the end
                if (i >= 0 && i < n) face.push_back ((int) i);
                r = skipWord (e, lineEnd);                           // the /vt/vn part
            }
            for (size_t k = 2; k < face.size(); ++k)
            {
                if ((int) mesh->tris.size() >= kMaxTris) { ++mesh->dropped; continue; }
                mesh->tris.push_back ({ verts[(size_t) face[0]], verts[(size_t) face[k - 1]], verts[(size_t) face[k]], colour });
            }
        }
        else if (lineEnd - q > 7 && std::strncmp (q, "mtllib", 6) == 0)
        {
            const auto name = juce::String (juce::CharPointer_UTF8 (q + 6), juce::CharPointer_UTF8 (lineEnd)).trim();
            auto m = readMtl (file.getSiblingFile (name));
            mtl.insert (m.begin(), m.end());
        }
        else if (lineEnd - q > 7 && std::strncmp (q, "usemtl", 6) == 0)
        {
            const auto name = juce::String (juce::CharPointer_UTF8 (q + 6), juce::CharPointer_UTF8 (lineEnd)).trim();
            const auto it = mtl.find (name);
            colour = it != mtl.end() ? it->second : stone;
        }
        p = lineEnd + 1;
    }

    if (mesh->tris.empty()) { error = juce::String::fromUTF8 ("Keine Fl\xc3\xa4" "chen in ") + file.getFileName(); return nullptr; }

    // floor at z = 0, corner at x = y = 0
    V3 lo { 1.0e30f, 1.0e30f, 1.0e30f }, hi { -1.0e30f, -1.0e30f, -1.0e30f };
    for (const auto& t : mesh->tris)
        for (const auto* v : { &t.a, &t.b, &t.c })
        {
            lo = { juce::jmin (lo.x, v->x), juce::jmin (lo.y, v->y), juce::jmin (lo.z, v->z) };
            hi = { juce::jmax (hi.x, v->x), juce::jmax (hi.y, v->y), juce::jmax (hi.z, v->z) };
        }
    for (auto& t : mesh->tris) { t.a = t.a - lo; t.b = t.b - lo; t.c = t.c - lo; }
    mesh->size = hi - lo;
    if (mesh->dropped > 0)
        mesh->message = juce::String (mesh->dropped) + juce::String::fromUTF8 (" Dreiecke weggelassen (h\xc3\xb6" "chstens ") + juce::String (kMaxTris) + ")";
    return mesh;
}

juce::String makeTestObj()
{
    // a small chapel: floor, four walls with a doorway, an altar, two rows of columns (z up, metres)
    juce::String s ("# Rysum test chapel\nmtllib none.mtl\n");
    int base = 0;
    auto box = [&] (float x0, float y0, float z0, float x1, float y1, float z1)
    {
        const float xs[] { x0, x1 }, ys[] { y0, y1 }, zs[] { z0, z1 };
        for (int i = 0; i < 8; ++i)
            s << "v " << xs[i & 1] << " " << ys[(i >> 1) & 1] << " " << zs[(i >> 2) & 1] << "\n";
        const int f[6][4] { { 0, 1, 3, 2 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 3, 7, 5 } };
        for (auto& q : f) s << "f " << base + q[0] + 1 << "/1/1 " << base + q[1] + 1 << " " << base + q[2] + 1 << " " << base + q[3] + 1 << "\n";
        base += 8;
    };
    box (0, 0, -0.2f, 12, 24, 0);                // floor
    box (-0.4f, 0, 0, 0, 24, 9);                 // left wall
    box (12, 0, 0, 12.4f, 24, 9);                // right wall
    box (0, 24, 0, 12, 24.4f, 9);                // front wall
    box (0, -0.4f, 0, 5, 0, 9);                  // back wall with a doorway
    box (7, -0.4f, 0, 12, 0, 9);
    box (5, -0.4f, 3, 7, 0, 9);
    box (4.5f, 20, 0, 7.5f, 21.5f, 1.1f);        // altar
    for (int i = 0; i < 4; ++i)
    {
        box (2.6f, 4.0f + 4.5f * (float) i, 0, 3.2f, 4.6f + 4.5f * (float) i, 8);
        box (8.8f, 4.0f + 4.5f * (float) i, 0, 9.4f, 4.6f + 4.5f * (float) i, 8);
    }
    return s;
}

// ---------------------------------------------------------------------------
// the renderer
// ---------------------------------------------------------------------------
static constexpr float kNear = 0.06f;

void Raster::setSize (int nw, int nh)
{
    nw = juce::jmax (8, nw); nh = juce::jmax (8, nh);
    if (nw == w && nh == h) return;
    w = nw; h = nh;
    img = juce::Image (juce::Image::ARGB, w, h, false);
    pix.assign ((size_t) (w * h), 0u);
    zbuf.assign ((size_t) (w * h), 0.0f);
}

void Raster::setCamera (V3 e, float yawDeg, float pitchDeg, float fovDeg)
{
    eye = e;
    const float ya = juce::degreesToRadians (yawDeg), pa = juce::degreesToRadians (juce::jlimit (-89.0f, 89.0f, pitchDeg));
    fwd = { -std::sin (ya) * std::cos (pa), std::cos (ya) * std::cos (pa), std::sin (pa) };
    right = { std::cos (ya), std::sin (ya), 0.0f };
    up = cross (right, fwd);
    focal = 0.5f * (float) w / std::tan (0.5f * juce::degreesToRadians (juce::jlimit (30.0f, 120.0f, fovDeg)));
    lightDir = normalised ({ 0.35f, -0.25f, 0.9f });
}

void Raster::clear (juce::Colour sky, juce::Colour fog, float fogDistance)
{
    fogR = fog.getFloatRed(); fogG = fog.getFloatGreen(); fogB = fog.getFloatBlue();
    fogInv = 1.0f / juce::jmax (1.0f, fogDistance);
    // a dim vault: brighter towards the horizon
    for (int y = 0; y < h; ++y)
    {
        const float t = juce::jlimit (0.0f, 1.0f, (float) y / (float) h);
        const auto c = sky.interpolatedWith (fog, juce::jlimit (0.0f, 1.0f, 1.0f - std::abs (t - 0.5f) * 2.0f));
        std::fill_n (pix.data() + (size_t) y * (size_t) w, (size_t) w, c.getARGB() | 0xff000000u);
    }
    std::fill (zbuf.begin(), zbuf.end(), 0.0f);
    trianglesDrawn = 0;
}

Raster::VV Raster::toView (V3 p) const noexcept
{
    const V3 d = p - eye;
    return { dot (d, right), dot (d, up), dot (d, fwd) };
}

bool Raster::project (V3 p, float& sx, float& sy, float& depth) const noexcept
{
    const auto v = toView (p);
    if (v.z < kNear) return false;
    sx = 0.5f * (float) w + v.x / v.z * focal;
    sy = 0.5f * (float) h - v.y / v.z * focal;
    depth = v.z;
    return true;
}

float Raster::depthAt (int x, int y) const noexcept
{
    if (x < 0 || y < 0 || x >= w || y >= h) return 0.0f;
    const float iz = zbuf[(size_t) y * (size_t) w + (size_t) x];
    return iz > 0.0f ? 1.0f / iz : 0.0f;
}

bool Raster::projectLine (V3 a, V3 b, juce::Point<float>& pa, juce::Point<float>& pb) const noexcept
{
    auto va = toView (a), vb = toView (b);
    if (va.z < kNear && vb.z < kNear) return false;
    auto clipTo = [] (VV& in, const VV& o)
    {
        const float t = (kNear - in.z) / (o.z - in.z);
        in = { in.x + (o.x - in.x) * t, in.y + (o.y - in.y) * t, kNear };
    };
    if (va.z < kNear) clipTo (va, vb);
    if (vb.z < kNear) clipTo (vb, va);
    pa = { 0.5f * (float) w + va.x / va.z * focal, 0.5f * (float) h - va.y / va.z * focal };
    pb = { 0.5f * (float) w + vb.x / vb.z * focal, 0.5f * (float) h - vb.y / vb.z * focal };
    return true;
}

void Raster::drawTri (const Tri& t, float emissive)
{
    const V3 n = normalised (cross (t.b - t.a, t.c - t.a));
    const V3 toEye = normalised (eye - (t.a + t.b + t.c) * (1.0f / 3.0f));
    // seen from both sides: the walls are looked at from inside, and OBJ windings are not to be trusted
    const float bright = juce::jlimit (0.0f, 1.6f, 0.26f + 0.46f * std::abs (dot (n, lightDir)) + 0.28f * std::abs (dot (n, toEye)) + emissive);
    const VV v[3] { toView (t.a), toView (t.b), toView (t.c) };
    fillClipped (v, 3, t.colour.getARGB(), bright);
}

void Raster::fillClipped (const VV* v, int n, uint32_t rgb, float bright)
{
    // clip at the near plane (Sutherland-Hodgman against one plane): the camera stands inside the geometry
    VV out[8];
    int m = 0;
    for (int i = 0; i < n && m < 7; ++i)
    {
        const VV& a = v[i];
        const VV& b = v[(i + 1) % n];
        const bool ina = a.z >= kNear, inb = b.z >= kNear;
        if (ina) out[m++] = a;
        if (ina != inb)
        {
            const float t = (kNear - a.z) / (b.z - a.z);
            out[m++] = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, kNear };
        }
    }
    if (m < 3) return;
    const float r = (float) ((rgb >> 16) & 0xff) / 255.0f * bright, g = (float) ((rgb >> 8) & 0xff) / 255.0f * bright,
                bl = (float) (rgb & 0xff) / 255.0f * bright;
    for (int i = 2; i < m; ++i) rasterTri (out[0], out[i - 1], out[i], r, g, bl);
    ++trianglesDrawn;
}

void Raster::rasterTri (VV a, VV b, VV c, float r, float g, float bl)
{
    const float hw = 0.5f * (float) w, hh = 0.5f * (float) h;
    float x0 = hw + a.x / a.z * focal, y0 = hh - a.y / a.z * focal, z0 = 1.0f / a.z;
    float x1 = hw + b.x / b.z * focal, y1 = hh - b.y / b.z * focal, z1 = 1.0f / b.z;
    float x2 = hw + c.x / c.z * focal, y2 = hh - c.y / c.z * focal, z2 = 1.0f / c.z;
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (std::abs (area) < 1.0e-6f) return;
    if (area < 0.0f) { std::swap (x1, x2); std::swap (y1, y2); std::swap (z1, z2); area = -area; }
    const int minX = juce::jmax (0, (int) std::floor (juce::jmin (x0, x1, x2)));
    const int maxX = juce::jmin (w - 1, (int) std::ceil (juce::jmax (x0, x1, x2)));
    const int minY = juce::jmax (0, (int) std::floor (juce::jmin (y0, y1, y2)));
    const int maxY = juce::jmin (h - 1, (int) std::ceil (juce::jmax (y0, y1, y2)));
    if (minX > maxX || minY > maxY) return;
    const float inv = 1.0f / area;
    // edge functions (barycentric weights), stepped per pixel
    const float b0 = (x2 - x1) * inv, b1 = (x0 - x2) * inv, b2 = (x1 - x0) * inv;     // per y
    const float px = (float) minX + 0.5f, py = (float) minY + 0.5f;
    float e0r = ((x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)) * inv;
    float e1r = ((x0 - x2) * (py - y2) - (y0 - y2) * (px - x2)) * inv;
    float e2r = ((x1 - x0) * (py - y0) - (y1 - y0) * (px - x0)) * inv;
    const float s0 = -(y2 - y1) * inv, s1 = -(y0 - y2) * inv, s2 = -(y1 - y0) * inv;     // per x
    for (int y = minY; y <= maxY; ++y)
    {
        float e0 = e0r, e1 = e1r, e2 = e2r;
        uint32_t* row = pix.data() + (size_t) y * (size_t) w;
        float* zr = zbuf.data() + (size_t) y * (size_t) w;
        for (int x = minX; x <= maxX; ++x)
        {
            if (e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f)
            {
                const float iz = e0 * z0 + e1 * z1 + e2 * z2;
                if (iz > zr[x])
                {
                    zr[x] = iz;
                    const float d = 1.0f / iz;
                    const float f = d * fogInv / (1.0f + d * fogInv);          // fog: cheap, no exp per pixel
                    const float rr = r + (fogR - r) * f, gg = g + (fogG - g) * f, bb = bl + (fogB - bl) * f;
                    row[x] = 0xff000000u | ((uint32_t) juce::jlimit (0, 255, (int) (rr * 255.0f)) << 16)
                                         | ((uint32_t) juce::jlimit (0, 255, (int) (gg * 255.0f)) << 8)
                                         | (uint32_t) juce::jlimit (0, 255, (int) (bb * 255.0f));
                }
            }
            e0 += s0; e1 += s1; e2 += s2;
        }
        e0r += b0; e1r += b1; e2r += b2;
    }
}

void Raster::drawPipe (V3 bottom, float length, float radius, juce::Colour metal, float glow)
{
    VV b = toView (bottom), t = toView (bottom + V3 { 0, 0, length });
    float f0 = 0.0f, f1 = 1.0f;              // the part of the pipe that is in front of the near plane
    if (b.z < kNear && t.z < kNear) return;
    if (b.z < kNear) { const float k = (kNear - b.z) / (t.z - b.z); b = { b.x + (t.x - b.x) * k, b.y + (t.y - b.y) * k, kNear }; f0 = k; }
    if (t.z < kNear) { const float k = (kNear - t.z) / (b.z - t.z); t = { t.x + (b.x - t.x) * k, t.y + (b.y - t.y) * k, kNear }; f1 = 1.0f - k; }
    const float hw = 0.5f * (float) w, hh = 0.5f * (float) h;
    const float bx = hw + b.x / b.z * focal, by = hh - b.y / b.z * focal, br = radius / b.z * focal;
    const float tx = hw + t.x / t.z * focal, ty = hh - t.y / t.z * focal, tr = radius / t.z * focal;
    const float izb = 1.0f / b.z, izt = 1.0f / t.z;
    const float mr = (float) ((metal.getARGB() >> 16) & 0xff) / 255.0f, mg = (float) ((metal.getARGB() >> 8) & 0xff) / 255.0f,
                mb = (float) (metal.getARGB() & 0xff) / 255.0f;
    float y0 = juce::jmin (by, ty), y1 = juce::jmax (by, ty);
    if (y1 - y0 < 1.0f) { y0 -= juce::jmax (br, tr); y1 += juce::jmax (br, tr); }       // seen from straight above / below: a disc
    const int iy0 = juce::jmax (0, (int) std::floor (y0)), iy1 = juce::jmin (h - 1, (int) std::ceil (y1));
    const float span = juce::jmax (1.0e-3f, by - ty);
    for (int y = iy0; y <= iy1; ++y)
    {
        // where along the pipe this row is (0 = bottom of the visible part, 1 = top)
        const float s = juce::jlimit (0.0f, 1.0f, ((float) y + 0.5f - by) / -span);
        const float frac = f0 + (f1 - f0) * s;                                  // along the whole pipe
        const float cx = bx + (tx - bx) * s;
        float rad = br + (tr - br) * s;
        if (frac < 0.16f) rad *= 0.3f + 0.7f * (frac / 0.16f);                  // the foot: a cone
        if (rad < 0.35f) rad = 0.35f;
        const float iz = izb + (izt - izb) * s;
        const float d = 1.0f / iz;
        const float fog = d * fogInv / (1.0f + d * fogInv);
        const int ix0 = juce::jmax (0, (int) std::floor (cx - rad)), ix1 = juce::jmin (w - 1, (int) std::ceil (cx + rad));
        uint32_t* row = pix.data() + (size_t) y * (size_t) w;
        float* zr = zbuf.data() + (size_t) y * (size_t) w;
        const bool mouth = frac > 0.17f && frac < 0.23f;
        for (int x = ix0; x <= ix1; ++x)
        {
            const float u = ((float) x + 0.5f - cx) / rad;
            if (u < -1.0f || u > 1.0f) continue;
            const float izp = iz * (1.0f + 0.02f * (1.0f - u * u));             // a little rounder in depth than flat
            if (izp <= zr[x]) continue;
            zr[x] = izp;
            const float body = std::sqrt (1.0f - u * u);
            const float hl = juce::jmax (0.0f, 1.0f - std::abs (u + 0.38f) * 4.0f);
            float k = 0.18f + 0.62f * body + 0.9f * hl * hl + 0.9f * glow;
            if (mouth && std::abs (u) < 0.45f) k *= 0.18f + 0.5f * glow;          // the mouth (labium): dark
            float rr = mr * k + 0.55f * glow, gg = mg * k + 0.32f * glow, bb = mb * k + 0.12f * glow;
            rr += (fogR - rr) * fog; gg += (fogG - gg) * fog; bb += (fogB - bb) * fog;
            row[x] = 0xff000000u | ((uint32_t) juce::jlimit (0, 255, (int) (rr * 255.0f)) << 16)
                                 | ((uint32_t) juce::jlimit (0, 255, (int) (gg * 255.0f)) << 8)
                                 | (uint32_t) juce::jlimit (0, 255, (int) (bb * 255.0f));
        }
    }
}

void Raster::finish()
{
    juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < h; ++y)
    {
        auto* line = bd.getLinePointer (y);
        std::memcpy (line, pix.data() + (size_t) y * (size_t) w, (size_t) w * 4);
    }
}
} // namespace rysum
