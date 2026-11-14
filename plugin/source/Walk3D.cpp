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
    const size_t len = mb.getSize();
    mb.append ("\n", 2);                     // a final line end and a terminating zero: the number readers stop there
    const char* p = static_cast<const char*> (mb.getData());
    const char* const end = p + len;
    // numbers with a decimal point whatever the host's locale is (strtof would read "1.5" as 1 with a decimal comma)
    auto readF = [] (const char*& s) -> float
    {
        while (*s == ' ' || *s == '\t') ++s;
        juce::CharPointer_ASCII c (s);
        const double v = juce::CharacterFunctions::readDoubleValue (c);
        s = c.getAddress();
        return (float) v;
    };

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
            const char* e = q + 2;
            const float x = readF (e);
            const float y = readF (e);
            const float z = readF (e);
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
// materials from the acoustics
// ---------------------------------------------------------------------------
Material wallMaterial (float aw, float hi, float scatter)
{
    // the Saal page's materials (absorption walls, high damping): the nearest one decides the pattern
    struct P { Material::Kind k; float aw, hi, mul, gloss; };
    static const P presets[] { { Material::stone, 0.02f, 0.30f, 1.0f, 0.18f }, { Material::plaster, 0.05f, 0.40f, 1.05f, 0.05f },
                               { Material::wood, 0.12f, 0.55f, 0.78f, 0.25f }, { Material::glass, 0.04f, 0.10f, 0.95f, 0.9f },
                               { Material::curtain, 0.50f, 0.85f, 0.62f, 0.0f }, { Material::mirror, 0.0f, 0.0f, 1.12f, 1.0f } };
    const P* best = &presets[0];
    float bd = 1.0e9f;
    for (const auto& p : presets)
    {
        const float da = (aw - p.aw) / 0.05f, dh = (hi - p.hi) / 0.12f;
        const float d = da * da + dh * dh;
        if (d < bd) { bd = d; best = &p; }
    }
    Material m;
    m.kind = best->k;
    // the more a wall swallows, the darker it is; high damping makes it duller
    m.tone = juce::jlimit (0.12f, 1.0f, (0.92f - 0.62f * std::sqrt (juce::jlimit (0.0f, 1.0f, aw))) * (1.0f - 0.12f * hi) * best->mul);
    m.rough = 0.03f + 0.35f * juce::jlimit (0.0f, 1.0f, scatter);
    m.gloss = best->gloss * (1.0f - aw) * (1.0f - 0.6f * hi) * (1.0f - 0.5f * scatter);
    return m;
}

Material floorMaterial (float af, float hi, float scatter, bool ceiling)
{
    Material m;
    af = juce::jlimit (0.0f, 1.0f, af);
    if (ceiling) m.kind = af < 0.18f ? Material::plaster : (af < 0.32f ? Material::boards : Material::carpet);
    else         m.kind = af < 0.12f ? Material::slabs : (af < 0.3f ? Material::boards : Material::carpet);
    m.tone = juce::jlimit (0.1f, 1.0f, (0.78f - 0.6f * std::sqrt (af)) * (1.0f - 0.1f * hi) * (ceiling ? 0.78f : 1.0f));   // (the ceiling darker than the floor, but its pattern stays readable)
    m.rough = 0.04f + 0.3f * juce::jlimit (0.0f, 1.0f, scatter);
    m.gloss = (m.kind == Material::slabs ? 0.35f : (m.kind == Material::boards ? 0.2f : 0.0f)) * (1.0f - af) * (1.0f - 0.6f * hi);
    return m;
}

juce::String materialName (Material::Kind k)
{
    switch (k)
    {
        case Material::stone:   return "Stein";
        case Material::plaster: return "Putz";
        case Material::wood:    return "Holz";
        case Material::glass:   return "Glas";
        case Material::curtain: return "Vorhang";
        case Material::mirror:  return "Spiegel";
        case Material::slabs:   return "Steinplatten";
        case Material::boards:  return "Dielen";
        case Material::carpet:  return "Teppich";
        default:                return {};
    }
}

// ---------------------------------------------------------------------------
// procedural textures (u, v in metres on the surface)
// ---------------------------------------------------------------------------
namespace
{
// (fast stand-ins: the CRT's floor / sin cost more than all the rest of a pixel)
inline float ffloor (float x) noexcept { const int i = (int) x; return (float) (i - (x < (float) i ? 1 : 0)); }
inline float fsin (float x) noexcept
{
    // sine to about 0.1 %: wrapped to -pi..pi, a parabola with one correction
    constexpr float tp = 1.0f / juce::MathConstants<float>::twoPi;
    x = x * tp;
    x -= ffloor (x + 0.5f);                   // -0.5 .. 0.5 turns
    const float y = 8.0f * x - 16.0f * x * std::abs (x);
    return 0.225f * (y * std::abs (y) - y) + y;
}
inline float fcos (float x) noexcept { return fsin (x + 0.5f * juce::MathConstants<float>::pi); }
inline float hash2 (int x, int y) noexcept
{
    uint32_t h = (uint32_t) x * 374761393u + (uint32_t) y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float) ((h ^ (h >> 16)) & 0xffffff) / 16777216.0f;
}
/** value noise from a table (256 x 256, repeating): a few loads instead of four hashes per pixel */
struct NoiseTable
{
    float t[256 * 256];
    NoiseTable() { for (int y = 0; y < 256; ++y) for (int x = 0; x < 256; ++x) t[y * 256 + x] = hash2 (x, y); }
};
const NoiseTable& noiseTable() { static const NoiseTable n; return n; }
inline float vnoise (float u, float v) noexcept
{
    static const float* const tab = noiseTable().t;
    const float fu = ffloor (u), fv = ffloor (v);
    const int iu = (int) fu & 255, iv = (int) fv & 255, iu1 = (iu + 1) & 255, iv1 = (iv + 1) & 255;
    float a = u - fu, b = v - fv;
    a = a * a * (3.0f - 2.0f * a); b = b * b * (3.0f - 2.0f * b);
    const float h00 = tab[iv * 256 + iu], h10 = tab[iv * 256 + iu1], h01 = tab[iv1 * 256 + iu], h11 = tab[iv1 * 256 + iu1];
    const float top = h00 + (h10 - h00) * a;
    return top + ((h01 + (h11 - h01) * a) - top) * b;
}
inline float fract (float x) noexcept { return x - ffloor (x); }

float texture (const Material& m, float u, float v) noexcept
{
    const float r = m.rough;
    switch (m.kind)
    {
        case Material::stone:
        {
            // ashlar: courses of 32 cm, blocks of 62 cm, every other course offset; the mortar dark
            const float row = ffloor (v / 0.32f);
            const float uu = u + ((int) row & 1) * 0.31f;
            const float col = ffloor (uu / 0.62f);
            const float fu = (uu - col * 0.62f), fv = (v - row * 0.32f);
            if (fu < 0.018f || fv < 0.018f) return 0.5f;
            return (0.84f + 0.32f * hash2 ((int) col, (int) row)) * (1.0f + r * 1.4f * (vnoise (u * 7.0f, v * 7.0f) - 0.5f));
        }
        case Material::plaster:
            return 1.0f + r * 1.3f * (vnoise (u * 0.7f, v * 0.7f) - 0.5f) + 0.35f * r * (vnoise (u * 11.0f, v * 11.0f) - 0.5f);
        case Material::wood:
        {
            // upright panelling: boards of 20 cm, dark seams, grain
            const float col = ffloor (u / 0.2f);
            const float fu = u - col * 0.2f;
            if (fu < 0.008f) return 0.42f;
            const float grain = 0.9f + 0.1f * fsin (u * 95.0f + 7.0f * vnoise (col * 3.1f + 0.3f, v * 1.4f));
            return (0.8f + 0.3f * hash2 ((int) col, 17)) * grain * (1.0f + r * (vnoise (u * 20.0f, v * 3.0f) - 0.5f));
        }
        case Material::glass:
        {
            // leaded panes; the glass a little brighter where it catches the light
            const float fu = fract (u / 0.45f) * 0.45f, fv = fract (v / 0.7f) * 0.7f;
            if (fu < 0.022f || fv < 0.022f) return 0.25f;
            return 1.12f + 0.16f * fsin (v * 1.3f + u * 0.7f) + 0.1f * r * (vnoise (u * 4.0f, v * 4.0f) - 0.5f);
        }
        case Material::curtain:
        {
            // heavy folds
            const float f = 0.5f + 0.5f * fcos (u * juce::MathConstants<float>::twoPi / 0.32f + 0.6f * fsin (v * 0.7f));
            return (0.5f + 0.55f * (f * std::sqrt (f))) * (1.0f + 0.5f * r * (vnoise (u * 30.0f, v * 30.0f) - 0.5f));
        }
        case Material::mirror:
        {
            const float fu = fract (u / 1.2f) * 1.2f;
            if (fu < 0.03f) return 0.55f;
            return 1.22f + 0.22f * fsin (v * 0.55f + 0.4f);
        }
        case Material::slabs:
        {
            const float cu = ffloor (u / 0.8f), cv = ffloor (v / 0.8f);
            const float fu = u - cu * 0.8f, fv = v - cv * 0.8f;
            if (fu < 0.012f || fv < 0.012f) return 0.55f;
            return (0.78f + 0.4f * hash2 ((int) cu, (int) cv)) * (1.0f + r * 1.2f * (vnoise (u * 5.0f, v * 5.0f) - 0.5f));
        }
        case Material::boards:
        {
            // floor boards of 16 cm along the room, joints every 2.4 m (offset per board)
            const float row = ffloor (v / 0.16f);
            const float fv = v - row * 0.16f;
            const float uu = u + hash2 ((int) row, 5) * 2.4f;
            const float fu = fract (uu / 2.4f) * 2.4f;
            if (fv < 0.006f || fu < 0.008f) return 0.45f;
            const float grain = 0.9f + 0.1f * fsin (v * 120.0f + 6.0f * vnoise (uu * 1.2f, row * 2.7f));
            return (0.82f + 0.3f * hash2 ((int) row, (int) ffloor (uu / 2.4f))) * grain;
        }
        case Material::carpet:
            return 0.9f + 0.2f * (vnoise (u * 40.0f, v * 40.0f) - 0.5f) + 0.12f * (vnoise (u * 2.0f, v * 2.0f) - 0.5f);
        case Material::metal:
        {
            // a pipe: u = around (0 = towards the listener), v = along (0 = foot tip, 1 = top). The mouth, the lip.
            float uw = fract (u + 0.5f) - 0.5f;
            uw = std::abs (uw);
            if (uw < 0.075f && v > 0.175f && v < 0.215f) return 0.1f;
            if (uw < 0.085f && v >= 0.215f && v < 0.228f) return 0.6f;
            const float foot = v < 0.16f ? 0.88f : 1.0f;
            return foot * (1.0f + 0.07f * (vnoise (u * 40.0f, v * 70.0f) - 0.5f));
        }
        case Material::dark:  return 0.12f;
        default:              return 1.0f + 0.1f * r * (vnoise (u * 3.0f, v * 3.0f) - 0.5f);
    }
}
} // namespace

// ---------------------------------------------------------------------------
// the renderer
// ---------------------------------------------------------------------------
static constexpr float kNear = 0.06f;

Raster::Raster()
{
    const int hc = (int) std::thread::hardware_concurrency();
    int n = juce::jlimit (0, 6, hc - 2);                 // (leave room for the audio thread and the host)
    const auto forced = juce::SystemStats::getEnvironmentVariable ("RYSUM_RENDER_THREADS", {});
    if (forced.isNotEmpty()) n = juce::jlimit (0, 16, forced.getIntValue());
    for (int i = 0; i < n; ++i) workers.emplace_back ([this] { workerLoop(); });
}

Raster::~Raster()
{
    { std::lock_guard<std::mutex> l (mx); quit = true; }
    cvWork.notify_all();
    for (auto& t : workers) t.join();
}

void Raster::workerLoop()
{
    int seen = 0;
    while (true)
    {
        const std::function<void (int)>* fn = nullptr;
        int count = 0;
        {
            std::unique_lock<std::mutex> l (mx);
            cvWork.wait (l, [&] { return quit || generation != seen; });
            if (quit) return;
            seen = generation;
            fn = job; count = jobCount;
        }
        for (int i = nextJob.fetch_add (1); i < count; i = nextJob.fetch_add (1)) (*fn) (i);
        {
            std::lock_guard<std::mutex> l (mx);
            if (--busy == 0) cvDone.notify_one();
        }
    }
}

void Raster::runParallel (int jobs, const std::function<void (int)>& fn)
{
    if (workers.empty() || jobs < 2) { for (int i = 0; i < jobs; ++i) fn (i); return; }
    {
        std::lock_guard<std::mutex> l (mx);
        job = &fn; jobCount = jobs; nextJob = 0; busy = (int) workers.size(); ++generation;
    }
    cvWork.notify_all();
    for (int i = nextJob.fetch_add (1); i < jobs; i = nextJob.fetch_add (1)) fn (i);
    std::unique_lock<std::mutex> l (mx);
    cvDone.wait (l, [&] { return busy == 0; });
    job = nullptr;
}

void Raster::setSize (int nw, int nh)
{
    nw = juce::jmax (8, nw); nh = juce::jmax (8, nh);
    if (nw == w && nh == h) return;
    w = nw; h = nh;
    img = juce::Image (juce::Image::ARGB, w, h, false);
    const size_t n = (size_t) (w * h);
    // (resized, never shrunk in memory: stepping the resolution down and up again allocates nothing)
    pix.resize (n, 0u);
    for (auto* b : { &zbuf, &gu, &gv, &gb, &ge, &gt }) b->resize (n, 0.0f);
    gm.resize (n, 0);
    gi.resize (n, 0);
    bins.resize ((size_t) ((h + kBandRows - 1) / kBandRows));
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

void Raster::begin (float fog, float fogDistance, float grainAmt, uint32_t frame)
{
    fogTone = fog;
    fogInv = 1.0f / juce::jmax (1.0f, fogDistance);
    grain = grainAmt;
    frameNo = frame;
    tris.clear();
    for (auto& b : bins) b.clear();
    trianglesDrawn = 0;
}

void Raster::setMaterial (int slot, const Material& m) { mats[(size_t) juce::jlimit (0, 31, slot)] = m; }

void Raster::setLamps (const std::vector<Lamp>& lamps, float amb)
{
    ambient = amb;
    lampsV.clear();
    dark = amb < 0.999f || ! lamps.empty();
    if (! dark) return;
    // the strongest ones (a lamp far behind the eye still lights what is seen: all count, by their power)
    std::vector<const Lamp*> order;
    for (const auto& l : lamps) if (l.power > 1.0e-4f) order.push_back (&l);
    std::sort (order.begin(), order.end(), [] (const Lamp* a, const Lamp* b) { return a->power > b->power; });
    if ((int) order.size() > kMaxLamps) order.resize ((size_t) kMaxLamps);
    for (const auto* l : order)
    {
        const auto v = toView (l->at);
        lampsV.insert (lampsV.end(), { v.x, v.y, v.z, l->power, 1.0f / juce::jmax (0.01f, l->reach * l->reach) });
    }
}

Raster::VV Raster::toView (V3 p) const noexcept
{
    const V3 d = p - eye;
    return { dot (d, right), dot (d, up), dot (d, fwd), 0, 0, 0, 0 };
}

float Raster::viewDepth (V3 p) const noexcept { return dot (p - eye, fwd); }

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
        in = { in.x + (o.x - in.x) * t, in.y + (o.y - in.y) * t, kNear, 0, 0, 0, 0 };
    };
    if (va.z < kNear) clipTo (va, vb);
    if (vb.z < kNear) clipTo (vb, va);
    pa = { 0.5f * (float) w + va.x / va.z * focal, 0.5f * (float) h - va.y / va.z * focal };
    pb = { 0.5f * (float) w + vb.x / vb.z * focal, 0.5f * (float) h - vb.y / vb.z * focal };
    return true;
}

float Raster::light (V3 p, V3 n, const Material& m) const noexcept
{
    const V3 E = normalised (eye - p);
    if (dot (n, E) < 0.0f) n = n * -1.0f;                      // seen from both sides
    const float ne = dot (n, E);
    float b = 0.24f + 0.42f * std::abs (dot (n, lightDir)) + 0.3f * ne;
    if (m.gloss > 0.0f)
    {
        const V3 H = normalised (lightDir + E);
        const float nh = juce::jmax (0.0f, dot (n, H));
        const float s1 = nh * nh, s2 = s1 * s1, s4 = s2 * s2, s8 = s4 * s4;          // ^16
        const float e2 = ne * ne, e4 = e2 * e2, e8 = e4 * e4, e16 = e8 * e8;         // ^32: a highlight towards the eye
        b += m.gloss * (0.7f * s8 * s8 + 0.35f * e16);
    }
    return b;
}

void Raster::triangle (const Vert& a, const Vert& b, const Vert& c, int slot, float tone, float emissive)
{
    const Vert* vs[3] { &a, &b, &c };
    VV v[3];
    for (int i = 0; i < 3; ++i) v[i] = toView (vs[i]->p);
    if (v[0].z < kNear && v[1].z < kNear && v[2].z < kNear) return;
    {
        const float kx = 0.5f * (float) w / focal * 1.02f, ky = 0.5f * (float) h / focal * 1.02f;
        auto allOut = [&] (auto test) { return test (v[0]) && test (v[1]) && test (v[2]); };
        if (allOut ([&] (const VV& q) { return q.x > q.z * kx; }) || allOut ([&] (const VV& q) { return q.x < -q.z * kx; })
            || allOut ([&] (const VV& q) { return q.y > q.z * ky; }) || allOut ([&] (const VV& q) { return q.y < -q.z * ky; })) return;
    }
    const auto& m = mats[(size_t) juce::jlimit (0, 31, slot)];
    for (int i = 0; i < 3; ++i)
    {
        v[i].b = light (vs[i]->p, vs[i]->n, m);
        v[i].u = vs[i]->u; v[i].v = vs[i]->v; v[i].e = emissive;
    }
    // the face's normal in view space, turned towards the eye (the lamps light the side that is seen)
    const V3 p0 { v[0].x, v[0].y, v[0].z };
    V3 fn = normalised (cross (V3 { v[1].x, v[1].y, v[1].z } - p0, V3 { v[2].x, v[2].y, v[2].z } - p0));
    if (dot (fn, p0) > 0.0f) fn = fn * -1.0f;
    const float nrm[3] { fn.x, fn.y, fn.z };
    emitClipped (v, 3, slot, tone, nrm);
}

void Raster::flatTriangle (V3 a, V3 b, V3 c, int slot, float tone, float emissive)
{
    const V3 n = normalised (cross (b - a, c - a));
    const float ax = std::abs (n.x), ay = std::abs (n.y), az = std::abs (n.z);
    auto uv = [&] (V3 p, float& u, float& v)
    {
        if (az >= ax && az >= ay) { u = p.x; v = p.y; }          // floors, ceilings
        else if (ax >= ay)        { u = p.y; v = p.z; }          // walls along y
        else                      { u = p.x; v = p.z; }          // walls along x
    };
    Vert va { a, n }, vb { b, n }, vc { c, n };
    uv (a, va.u, va.v); uv (b, vb.u, vb.v); uv (c, vc.u, vc.v);
    triangle (va, vb, vc, slot, tone, emissive);
}

void Raster::emitClipped (const VV* v, int n, int slot, float tone, const float* nrm)
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
            out[m++] = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, kNear, a.b + (b.b - a.b) * t,
                         a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t, a.e + (b.e - a.e) * t };
        }
    }
    if (m < 3) return;
    const float hw = 0.5f * (float) w, hh = 0.5f * (float) h;
    for (int i = 2; i < m; ++i)
    {
        const VV* q[3] { &out[0], &out[i - 1], &out[i] };
        ST s;
        float minX = 1.0e30f, maxX = -1.0e30f, minY = 1.0e30f, maxY = -1.0e30f;
        for (int k = 0; k < 3; ++k)
        {
            const float iz = 1.0f / q[k]->z;
            s.x[k] = hw + q[k]->x * iz * focal; s.y[k] = hh - q[k]->y * iz * focal; s.iz[k] = iz;
            s.b[k] = q[k]->b; s.uz[k] = q[k]->u * iz; s.vz[k] = q[k]->v * iz; s.e[k] = q[k]->e;
            minX = juce::jmin (minX, s.x[k]); maxX = juce::jmax (maxX, s.x[k]);
            minY = juce::jmin (minY, s.y[k]); maxY = juce::jmax (maxY, s.y[k]);
        }
        const float area = (s.x[1] - s.x[0]) * (s.y[2] - s.y[0]) - (s.x[2] - s.x[0]) * (s.y[1] - s.y[0]);
        if (std::abs (area) < 1.0e-7f || maxX < 0.0f || minX > (float) w || maxY < 0.0f || minY > (float) h) continue;
        s.minY = juce::jmax (0, (int) std::floor (minY));
        s.maxY = juce::jmin (h - 1, (int) std::ceil (maxY));
        if (s.minY > s.maxY) continue;
        s.slot = (uint8_t) slot;
        s.tone = tone;
        s.n[0] = nrm[0]; s.n[1] = nrm[1]; s.n[2] = nrm[2];
        const int idx = (int) tris.size();
        tris.push_back (s);
        for (int b = s.minY / kBandRows; b <= s.maxY / kBandRows; ++b) bins[(size_t) b].push_back (idx);
    }
    ++trianglesDrawn;
}

void Raster::rasterBand (int band)
{
    const int y0 = band * kBandRows, y1 = juce::jmin (h - 1, y0 + kBandRows - 1);
    std::fill (zbuf.begin() + (ptrdiff_t) y0 * w, zbuf.begin() + (ptrdiff_t) (y1 + 1) * w, 0.0f);
    for (int idx : bins[(size_t) band])
    {
        const ST& s = tris[(size_t) idx];
        float x0 = s.x[0], y0f = s.y[0], x1 = s.x[1], y1f = s.y[1], x2 = s.x[2], y2f = s.y[2];
        int i1 = 1, i2 = 2;
        float area = (x1 - x0) * (y2f - y0f) - (x2 - x0) * (y1f - y0f);
        if (area < 0.0f) { std::swap (x1, x2); std::swap (y1f, y2f); std::swap (i1, i2); area = -area; }
        const float inv = 1.0f / area;
        const int minX = juce::jmax (0, (int) std::floor (juce::jmin (x0, x1, x2)));
        const int maxX = juce::jmin (w - 1, (int) std::ceil (juce::jmax (x0, x1, x2)));
        const int ry0 = juce::jmax (y0, s.minY), ry1 = juce::jmin (y1, s.maxY);
        if (minX > maxX || ry0 > ry1) continue;
        // barycentric weights, stepped per pixel
        const float s0 = -(y2f - y1f) * inv, s1 = -(y0f - y2f) * inv, s2 = -(y1f - y0f) * inv;
        const float t0 = (x2 - x1) * inv, t1 = (x0 - x2) * inv, t2 = (x1 - x0) * inv;
        const float px = (float) minX + 0.5f, py = (float) ry0 + 0.5f;
        float e0r = ((x2 - x1) * (py - y1f) - (y2f - y1f) * (px - x1)) * inv;
        float e1r = ((x0 - x2) * (py - y2f) - (y0f - y2f) * (px - x2)) * inv;
        float e2r = ((x1 - x0) * (py - y0f) - (y1f - y0f) * (px - x0)) * inv;
        const float z0 = s.iz[0], z1 = s.iz[i1], z2 = s.iz[i2];
        for (int y = ry0; y <= ry1; ++y)
        {
            float e0 = e0r, e1 = e1r, e2 = e2r;
            const size_t row = (size_t) y * (size_t) w;
            for (int x = minX; x <= maxX; ++x)
            {
                if (e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f)
                {
                    const float iz = e0 * z0 + e1 * z1 + e2 * z2;
                    const size_t k = row + (size_t) x;
                    if (iz > zbuf[k])
                    {
                        zbuf[k] = iz;
                        if (depthOnly) { e0 += s0; e1 += s1; e2 += s2; continue; }
                        const float rz = 1.0f / iz;
                        gu[k] = (e0 * s.uz[0] + e1 * s.uz[i1] + e2 * s.uz[i2]) * rz;
                        gv[k] = (e0 * s.vz[0] + e1 * s.vz[i1] + e2 * s.vz[i2]) * rz;
                        gb[k] = e0 * s.b[0] + e1 * s.b[i1] + e2 * s.b[i2];
                        ge[k] = e0 * s.e[0] + e1 * s.e[i1] + e2 * s.e[i2];
                        gt[k] = s.tone;
                        gm[k] = s.slot;
                        gi[k] = idx;
                    }
                }
                e0 += s0; e1 += s1; e2 += s2;
            }
            e0r += t0; e1r += t1; e2r += t2;
        }
    }
}

void Raster::shadeBand (int band)
{
    const int y0 = band * kBandRows, y1 = juce::jmin (h - 1, y0 + kBandRows - 1);
    const float cx = 0.5f * (float) w, cy = 0.5f * (float) h, ir2 = 1.0f / (cx * cx + cy * cy);
    for (int y = y0; y <= y1; ++y)
    {
        const size_t row = (size_t) y * (size_t) w;
        // above / below everything: a dark vault, a little lighter at the horizon
        const float sky = fogTone * (0.55f + 0.45f * (1.0f - std::abs ((float) y / (float) h - 0.5f) * 2.0f));
        for (int x = 0; x < w; ++x)
        {
            const size_t k = row + (size_t) x;
            float l;
            const float iz = zbuf[k];
            if (iz <= 0.0f) l = sky;
            else
            {
                const auto& m = mats[gm[k]];
                const float d = 1.0f / iz;
                float lit = gb[k];
                if (dark)
                {
                    // light off: a faint rest of the usual light, and the lamps (the surface's facing, the lamp's
                    // distance: softened within its reach)
                    lit *= ambient;
                    const float* nf = tris[(size_t) gi[k]].n;
                    const float vx = ((float) x + 0.5f - cx) / focal * d, vy = (cy - ((float) y + 0.5f)) / focal * d;
                    for (size_t li = 0; li + 4 < lampsV.size(); li += 5)
                    {
                        const float lx = lampsV[li] - vx, ly = lampsV[li + 1] - vy, lz = lampsV[li + 2] - d;
                        const float d2 = lx * lx + ly * ly + lz * lz;
                        const float ndl = (nf[0] * lx + nf[1] * ly + nf[2] * lz) / std::sqrt (d2 + 1.0e-6f);
                        if (ndl > 0.0f) lit += lampsV[li + 3] * ndl / (1.0f + d2 * lampsV[li + 4]);
                    }
                }
                l = m.tone * gt[k] * lit * texture (m, gu[k], gv[k]) + ge[k];
                const float f = d * fogInv / (1.0f + d * fogInv);
                l += (fogTone - l) * f;
            }
            // vignette and grain (moving with every frame, like film)
            const float dx = (float) x - cx, dy = (float) y - cy;
            l *= 1.0f - 0.38f * (dx * dx + dy * dy) * ir2;
            const float gr = hash2 (x + (int) (frameNo * 7919u), y * 3 + (int) (frameNo * 104729u)) - 0.5f;
            l += grain * gr * (0.3f + 0.6f * juce::jmin (1.0f, l * 2.0f)) * (1.0f - 0.55f * juce::jmin (1.0f, l));   // (less on the highlights)
            // greys of an old print: ink-black to paper-white, the mids a touch warm
            l = juce::jlimit (0.0f, 1.0f, l);
            const float g2 = l * l * (3.0f - 2.0f * l) * 0.35f + l * 0.65f;       // a little more contrast
            const int r = (int) (13.0f + 222.0f * g2 + 6.0f * l * (1.0f - l));
            const int g = (int) (13.0f + 219.0f * g2 + 2.0f * l * (1.0f - l));
            const int b = (int) (12.0f + 214.0f * g2 - 5.0f * l * (1.0f - l));
            pix[k] = 0xff000000u | ((uint32_t) juce::jlimit (0, 255, r) << 16) | ((uint32_t) juce::jlimit (0, 255, g) << 8) | (uint32_t) juce::jlimit (0, 255, b);
        }
    }
}

void Raster::finishDepth()
{
    const int bands = (int) bins.size();
    const std::function<void (int)> depth = [this] (int b) { rasterBand (b); };
    const double tA = juce::Time::getMillisecondCounterHiRes();
    // (on this thread alone: it is little work, and waking the helpers for it costs more than it saves)
    depthOnly = true;
    for (int b = 0; b < bands; ++b) depth (b);
    depthOnly = false;
    rasterMs = (float) (juce::Time::getMillisecondCounterHiRes() - tA); shadeMs = 0.0f;
}

void Raster::exportGpu (std::vector<float>& verts, std::array<float, 32 * 4>& materials) const
{
    verts.resize (tris.size() * 36);
    const float sx = 2.0f / (float) w, sy = 2.0f / (float) h;
    float* o = verts.data();
    for (const auto& s : tris)
        for (int k = 0; k < 3; ++k)
        {
            const float rz = 1.0f / s.iz[k];
            *o++ = s.x[k] * sx - 1.0f; *o++ = 1.0f - s.y[k] * sy; *o++ = s.iz[k];
            *o++ = s.b[k]; *o++ = s.uz[k] * rz; *o++ = s.vz[k] * rz; *o++ = s.e[k];
            *o++ = s.tone; *o++ = (float) s.slot;
            *o++ = s.n[0]; *o++ = s.n[1]; *o++ = s.n[2];
        }
    for (size_t i = 0; i < 32; ++i)
    {
        materials[4 * i] = (float) mats[i].kind; materials[4 * i + 1] = mats[i].tone;
        materials[4 * i + 2] = mats[i].rough; materials[4 * i + 3] = 0.0f;
    }
}

void Raster::finish()
{
    const int bands = (int) bins.size();
    // every band drawn and then shaded at once, while its rows are still in the cache
    const std::function<void (int)> both = [this] (int b) { rasterBand (b); shadeBand (b); };
    const double tA = juce::Time::getMillisecondCounterHiRes();
    runParallel (bands, both);
    rasterMs = (float) (juce::Time::getMillisecondCounterHiRes() - tA); shadeMs = 0.0f;
    juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < h; ++y)
        std::memcpy (bd.getLinePointer (y), pix.data() + (size_t) y * (size_t) w, (size_t) w * 4);
}
} // namespace rysum
