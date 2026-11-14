#include "RoomModels.h"
#include <cmath>

namespace rysum
{
namespace
{
constexpr float kPi = 3.14159265f;

/** OBJ text from boxes, prisms and a few free shapes (z up, metres) */
struct Obj
{
    juce::MemoryOutputStream s;
    int n = 0; // vertices so far

    explicit Obj(const char *title)
    {
        s << "# Rysum: " << title << "\n# z up, metres\nmtllib " << builtInRoomMaterialFile() << "\n";
    }
    juce::String text()
    {
        return s.toString();
    }
    void mat(const char *name)
    {
        s << "usemtl " << name << "\n";
    }
    int v(float x, float y, float z)
    {
        s << "v " << juce::String(x, 4) << " " << juce::String(y, 4) << " " << juce::String(z, 4) << "\n";
        return ++n;
    }
    void quad(int a, int b, int c, int d)
    {
        s << "f " << a << " " << b << " " << c << " " << d << "\n";
    }
    void tri(int a, int b, int c)
    {
        s << "f " << a << " " << b << " " << c << "\n";
    }
    /** a solid with a bottom and a top of four corners each (bottom 0 .. 3 around, top 4 .. 7
     *  above them) */
    void hexa(const float p[8][3])
    {
        int i[8];
        for (int k = 0; k < 8; ++k)
            i[k] = v(p[k][0], p[k][1], p[k][2]);
        quad(i[0], i[1], i[2], i[3]);
        quad(i[4], i[5], i[6], i[7]);
        for (int k = 0; k < 4; ++k)
            quad(i[k], i[(k + 1) % 4], i[4 + (k + 1) % 4], i[4 + k]);
    }
    void box(float x0, float y0, float z0, float x1, float y1, float z1)
    {
        const float p[8][3]{{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                            {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
        hexa(p);
    }
    /** a level floor or lid */
    void slab(float x0, float y0, float x1, float y1, float z)
    {
        quad(v(x0, y0, z), v(x1, y0, z), v(x1, y1, z), v(x0, y1, z));
    }
    /** an upright prism with 'sides' sides (a round column, a well) */
    void prism(float cx, float cy, float r, float z0, float z1, int sides, float turn = 0.0f)
    {
        std::vector<int> lo, hi;
        for (int k = 0; k < sides; ++k)
        {
            const float a = turn + 2.0f * kPi * (float)k / (float)sides;
            lo.push_back(v(cx + r * std::cos(a), cy + r * std::sin(a), z0));
            hi.push_back(v(cx + r * std::cos(a), cy + r * std::sin(a), z1));
        }
        for (int k = 0; k < sides; ++k)
            quad(lo[(size_t)k], lo[(size_t)((k + 1) % sides)], hi[(size_t)((k + 1) % sides)], hi[(size_t)k]);
        const int cl = v(cx, cy, z0), ch = v(cx, cy, z1);
        for (int k = 0; k < sides; ++k)
        {
            tri(cl, lo[(size_t)k], lo[(size_t)((k + 1) % sides)]);
            tri(ch, hi[(size_t)k], hi[(size_t)((k + 1) % sides)]);
        }
    }
    /** a band between two rings (a dome's course): radius r0 at z0, r1 at z1 */
    void ring(float cx, float cy, float r0, float z0, float r1, float z1, int sides, float turn = 0.0f)
    {
        std::vector<int> lo, hi;
        for (int k = 0; k < sides; ++k)
        {
            const float a = turn + 2.0f * kPi * (float)k / (float)sides;
            lo.push_back(v(cx + r0 * std::cos(a), cy + r0 * std::sin(a), z0));
            hi.push_back(v(cx + r1 * std::cos(a), cy + r1 * std::sin(a), z1));
        }
        for (int k = 0; k < sides; ++k)
            quad(lo[(size_t)k], lo[(size_t)((k + 1) % sides)], hi[(size_t)((k + 1) % sides)], hi[(size_t)k]);
    }
    /** a level disc */
    void disc(float cx, float cy, float r, float z, int sides, float turn = 0.0f)
    {
        const int c = v(cx, cy, z);
        std::vector<int> e;
        for (int k = 0; k < sides; ++k)
        {
            const float a = turn + 2.0f * kPi * (float)k / (float)sides;
            e.push_back(v(cx + r * std::cos(a), cy + r * std::sin(a), z));
        }
        for (int k = 0; k < sides; ++k)
            tri(c, e[(size_t)k], e[(size_t)((k + 1) % sides)]);
    }
    /** a round column on a square base, with a square capital under 'top' */
    void column(float cx, float cy, float r, float top, int sides = 12)
    {
        const float b = 1.35f * r, c = 1.45f * r;
        box(cx - b, cy - b, 0.0f, cx + b, cy + b, 0.3f);
        prism(cx, cy, r, 0.3f, top - 0.4f, sides);
        box(cx - c, cy - c, top - 0.4f, cx + c, cy + c, top);
    }
    /** floor, four walls of thickness t (to height wallTop) and, if asked for, a roof over the
     *  inner room 0 .. W, 0 .. L, 0 .. H */
    void shell(float W, float L, float H, float t, bool roof, float wallTop = -1.0f)
    {
        const float top = wallTop > 0.0f ? wallTop : H;
        mat("boden");
        slab(-t, -t, W + t, L + t, 0.0f);
        mat("stein");
        box(-t, -t, 0.0f, 0.0f, L + t, top);
        box(W, -t, 0.0f, W + t, L + t, top);
        box(0.0f, -t, 0.0f, W, 0.0f, top);
        box(0.0f, L, 0.0f, W, L + t, top);
        if (roof)
        {
            mat("hell");
            box(-t, -t, H, W + t, L + t, H + 0.3f);
        }
    }
};

// ---------------------------------------------------------------------------
juce::String makeKapelle()
{
    // a small chapel: two rows of round columns carrying beams, pews left and right of the aisle,
    // steps up to the altar
    const float W = 12.0f, L = 24.0f, H = 9.0f, t = 0.4f;
    Obj o("Kapelle");
    o.mat("boden");
    o.slab(-t, -t, W + t, L + t, 0.0f);
    o.mat("stein");
    o.box(-t, -t, 0.0f, 0.0f, L + t, H);
    o.box(W, -t, 0.0f, W + t, L + t, H);
    o.box(0.0f, L, 0.0f, W, L + t, H);
    o.box(0.0f, -t, 0.0f, 5.0f, 0.0f, H); // the back wall with its doorway
    o.box(7.0f, -t, 0.0f, W, 0.0f, H);
    o.box(5.0f, -t, 3.0f, 7.0f, 0.0f, H);
    o.mat("dunkel");
    o.box(5.0f, -t, 0.0f, 7.0f, -t + 0.08f, 3.0f); // the door
    o.mat("hell");
    o.box(-t, -t, H, W + t, L + t, H + 0.3f);
    for (int i = 0; i < 4; ++i)
    {
        const float y = 4.5f + 4.5f * (float)i;
        o.mat("stein");
        o.column(2.9f, y, 0.32f, 7.0f);
        o.column(9.1f, y, 0.32f, 7.0f);
        o.box(0.0f, y - 0.3f, 0.0f, 0.25f, y + 0.3f, 8.4f); // pilasters on the walls
        o.box(W - 0.25f, y - 0.3f, 0.0f, W, y + 0.3f, 8.4f);
        o.mat("holz");
        o.box(0.0f, y - 0.2f, 8.4f, W, y + 0.2f, H); // ceiling beams
    }
    o.mat("stein");
    o.box(2.55f, 4.1f, 7.0f, 3.25f, 18.4f, 7.6f); // the beams the columns carry
    o.box(8.75f, 4.1f, 7.0f, 9.45f, 18.4f, 7.6f);
    o.mat("holz");
    for (int i = 0; i < 8; ++i)
    {
        const float y = 5.6f + 1.45f * (float)i;
        for (float x0 : {3.6f, 7.0f})
        {
            o.box(x0, y, 0.0f, x0 + 1.4f, y + 0.45f, 0.45f);        // the seat
            o.box(x0, y + 0.45f, 0.0f, x0 + 1.4f, y + 0.55f, 0.95f); // its back
        }
    }
    o.mat("hell");
    o.box(1.5f, 19.5f, 0.0f, 10.5f, L, 0.18f);
    o.box(3.0f, 20.5f, 0.18f, 9.0f, L, 0.36f);
    o.mat("stein");
    o.box(4.6f, 21.8f, 0.36f, 7.4f, 22.9f, 1.4f); // altar
    return o.text();
}

juce::String makeHallenkirche()
{
    // a hall church: a high nave between two rows of columns, lower aisles, a west gallery reached
    // by a stair along the left wall, steps up to the choir
    const float W = 21.0f, L = 40.0f, H = 15.0f, t = 0.6f;
    Obj o("Hallenkirche");
    o.shell(W, L, H, t, true);
    for (int i = 0; i < 5; ++i)
    {
        const float y = 7.5f + 6.0f * (float)i;
        o.mat("stein");
        o.column(6.5f, y, 0.55f, 10.6f, 14);
        o.column(14.5f, y, 0.55f, 10.6f, 14);
        o.mat("hell");
        o.box(7.0f, y - 0.3f, 13.8f, 14.0f, y + 0.3f, H); // arches across the nave (as beams)
    }
    o.mat("stein");
    o.box(6.0f, 0.0f, 10.6f, 7.0f, L, 12.0f); // the arcades the columns carry
    o.box(14.0f, 0.0f, 10.6f, 15.0f, L, 12.0f);
    o.mat("hell");
    o.box(0.0f, 0.0f, 12.0f, 6.0f, L, H); // above the aisles: they are lower than the nave
    o.box(15.0f, 0.0f, 12.0f, W, L, H);
    // the choir
    o.mat("hell");
    o.box(3.0f, 33.0f, 0.0f, 18.0f, L, 0.18f);
    o.box(4.5f, 34.2f, 0.18f, 16.5f, L, 0.36f);
    o.box(6.0f, 35.4f, 0.36f, 15.0f, L, 0.54f);
    o.mat("stein");
    o.box(8.5f, 37.0f, 0.54f, 12.5f, 38.2f, 1.6f);
    // the west gallery
    const float gz = 4.3f;
    o.mat("holz");
    o.box(0.0f, 0.0f, gz - 0.3f, W, 5.0f, gz);
    o.box(1.5f, 4.85f, gz, W, 5.0f, gz + 1.0f); // its balustrade (open where the stair arrives)
    o.mat("stein");
    for (float x : {3.5f, 8.5f, 12.5f, 17.5f})
        o.prism(x, 4.6f, 0.25f, 0.0f, gz - 0.3f, 10);
    // the stair: 21 steps along the left wall, up towards the back
    o.mat("holz");
    const int steps = 21;
    const float rise = gz / (float)steps, run = 0.3f;
    for (int i = 0; i < steps; ++i)
    {
        const float y1 = 5.0f + run * (float)(steps - i);
        o.box(0.0f, y1 - run, 0.0f, 1.4f, y1, rise * (float)(i + 1));
    }
    {
        // its rail (a wall that rises with the steps)
        const float ya = 5.0f, yb = 5.0f + run * (float)steps;
        const float p[8][3]{{1.4f, ya, 0.0f}, {1.5f, ya, 0.0f}, {1.5f, yb, 0.0f}, {1.4f, yb, 0.0f},
                            {1.4f, ya, gz + 1.0f}, {1.5f, ya, gz + 1.0f}, {1.5f, yb, 1.0f}, {1.4f, yb, 1.0f}};
        o.hexa(p);
    }
    return o.text();
}

juce::String makeKrypta()
{
    // a crypt: low, four by four short columns, ribs under the ceiling in both directions
    const float W = 13.0f, L = 17.0f, H = 4.2f, t = 0.7f;
    Obj o("Krypta");
    o.shell(W, L, H, t, true);
    for (int i = 0; i < 4; ++i)
    {
        const float x = 2.6f + 2.6f * (float)i, y = 3.4f + 3.4f * (float)i;
        o.mat("dunkel");
        o.box(x - 0.3f, 0.0f, 3.5f, x + 0.3f, L, H);
        o.box(0.0f, y - 0.3f, 3.5f, W, y + 0.3f, H);
        o.mat("stein");
        for (int j = 0; j < 4; ++j)
            o.column(x, 3.4f + 3.4f * (float)j, 0.38f, 3.5f, 10);
    }
    o.mat("hell");
    o.box(5.6f, 15.0f, 0.0f, 7.4f, 16.2f, 0.95f); // a tomb
    o.box(5.5f, 14.9f, 0.95f, 7.5f, 16.3f, 1.1f);
    return o.text();
}

juce::String makeRotunde()
{
    // a round hall: a ring of eight columns carrying a ring of beams, a dome above
    const float R = 9.0f, t = 0.7f, wallTop = 9.5f;
    const int sides = 20;
    const float cx = R, cy = R;
    Obj o("Rotunde");
    o.mat("boden");
    o.disc(cx, cy, R + t, 0.0f, sides);
    o.mat("stein");
    auto segment = [&](float r0, float r1, float a0, float a1, float z0, float z1)
    {
        const float p[8][3]{{cx + r0 * std::cos(a0), cy + r0 * std::sin(a0), z0},
                            {cx + r1 * std::cos(a0), cy + r1 * std::sin(a0), z0},
                            {cx + r1 * std::cos(a1), cy + r1 * std::sin(a1), z0},
                            {cx + r0 * std::cos(a1), cy + r0 * std::sin(a1), z0},
                            {cx + r0 * std::cos(a0), cy + r0 * std::sin(a0), z1},
                            {cx + r1 * std::cos(a0), cy + r1 * std::sin(a0), z1},
                            {cx + r1 * std::cos(a1), cy + r1 * std::sin(a1), z1},
                            {cx + r0 * std::cos(a1), cy + r0 * std::sin(a1), z1}};
        o.hexa(p);
    };
    for (int k = 0; k < sides; ++k)
        segment(R, R + t, 2.0f * kPi * (float)k / (float)sides, 2.0f * kPi * (float)(k + 1) / (float)sides,
                0.0f, wallTop);
    for (int k = 0; k < 8; ++k)
    {
        const float a = 2.0f * kPi * ((float)k + 0.5f) / 8.0f, b = 2.0f * kPi * ((float)k + 1.5f) / 8.0f;
        o.mat("stein");
        o.column(cx + 5.6f * std::cos(a), cy + 5.6f * std::sin(a), 0.45f, 7.6f);
        o.mat("hell");
        segment(5.25f, 5.95f, a, b, 7.6f, 8.4f);
    }
    o.mat("hell");
    o.ring(cx, cy, R, wallTop, 7.6f, 12.2f, sides);
    o.ring(cx, cy, 7.6f, 12.2f, 4.8f, 14.2f, sides);
    o.ring(cx, cy, 4.8f, 14.2f, 1.6f, 15.2f, sides);
    o.disc(cx, cy, 1.6f, 15.2f, sides);
    return o.text();
}

juce::String makeKreuzgang()
{
    // a cloister: a covered walk around a court, pillars and a low wall between walk and court
    // (one bay open on every side), the court's walls rising above the walk, a well
    const float S = 24.0f, t = 0.5f, w = 3.6f, walkH = 5.0f, top = 9.0f;
    Obj o("Kreuzgang");
    o.shell(S, S, top, t, false);
    o.mat("holz");
    o.box(0.0f, 0.0f, walkH, S, w, walkH + 0.3f); // the ceiling of the walk
    o.box(0.0f, S - w, walkH, S, S, walkH + 0.3f);
    o.box(0.0f, w, walkH, w, S - w, walkH + 0.3f);
    o.box(S - w, w, walkH, S, S - w, walkH + 0.3f);
    const float a = w, b = S - w, bay = (b - a) / 6.0f;
    o.mat("stein");
    // the court's walls above the arcades
    o.box(a, a, 4.2f, b, a + 0.5f, top);
    o.box(a, b - 0.5f, 4.2f, b, b, top);
    o.box(a, a + 0.5f, 4.2f, a + 0.5f, b - 0.5f, top);
    o.box(b - 0.5f, a + 0.5f, 4.2f, b, b - 0.5f, top);
    for (int i = 0; i <= 6; ++i)
    {
        const float q = a + bay * (float)i;
        const float lo = juce::jlimit(a, b - 0.5f, q - 0.25f);
        o.mat("stein");
        o.box(lo, a, 0.0f, lo + 0.5f, a + 0.5f, 4.2f); // pillars: south, north, west, east
        o.box(lo, b - 0.5f, 0.0f, lo + 0.5f, b, 4.2f);
        if (i > 0 && i < 6)
        {
            o.box(a, lo, 0.0f, a + 0.5f, lo + 0.5f, 4.2f);
            o.box(b - 0.5f, lo, 0.0f, b, lo + 0.5f, 4.2f);
        }
        if (i < 6 && i != 2)
        {
            // the low wall of this bay (bay 2 is the way into the court)
            o.mat("hell");
            const float q0 = q + 0.25f, q1 = q + bay - 0.25f;
            o.box(q0, a + 0.1f, 0.0f, q1, a + 0.4f, 0.6f);
            o.box(q0, b - 0.4f, 0.0f, q1, b - 0.1f, 0.6f);
            o.box(a + 0.1f, q0, 0.0f, a + 0.4f, q1, 0.6f);
            o.box(b - 0.4f, q0, 0.0f, b - 0.1f, q1, 0.6f);
        }
    }
    o.mat("hell");
    o.prism(12.0f, 16.5f, 1.1f, 0.0f, 0.9f, 10); // the well
    o.mat("dunkel");
    o.disc(12.0f, 16.5f, 0.85f, 0.905f, 10);
    return o.text();
}

juce::String makeTreppenturm()
{
    // a stair tower: five flights along the walls, one above the other, up to a landing 20 m
    // above the floor - the listener climbs, and hears the room from up there
    const float S = 9.0f, H = 23.0f, t = 0.8f, w = 1.5f, rise = 0.2f, run = 0.3f;
    const int steps = 20;
    Obj o("Treppenturm");
    o.shell(S, S, H, t, true);
    // flight k: where it starts (the inner corner of its first step), which way it goes, and
    // which way its wall lies
    struct Flight
    {
        float x, y;
        int dx, dy, wx, wy;
    };
    const Flight fl[4]{{w, w, 0, 1, -1, 0}, {w, S - w, 1, 0, 0, 1}, {S - w, S - w, 0, -1, 1, 0}, {S - w, w, -1, 0, 0, -1}};
    for (int k = 0; k < 5; ++k)
    {
        const auto &f = fl[k % 4];
        const float z0 = 4.0f * (float)k;
        o.mat("holz");
        for (int i = 0; i < steps; ++i)
        {
            const float a0 = run * (float)i, a1 = run * (float)(i + 1), z = z0 + rise * (float)(i + 1);
            const float xa = f.x + (float)f.dx * a0, xb = f.x + (float)f.dx * a1 + (float)f.wx * w,
                        ya = f.y + (float)f.dy * a0, yb = f.y + (float)f.dy * a1 + (float)f.wy * w;
            o.box(juce::jmin(xa, xb), juce::jmin(ya, yb), k == 0 ? 0.0f : z - rise, juce::jmax(xa, xb),
                  juce::jmax(ya, yb), z);
        }
        {
            // the rail on the open side, rising with the steps
            const float len = run * (float)steps, th = 0.08f;
            const float ax = f.x, ay = f.y, bx = f.x + (float)f.dx * len, by = f.y + (float)f.dy * len;
            const float ox = -(float)f.wx * th, oy = -(float)f.wy * th;
            const float za = z0, zb = z0 + rise * (float)steps;
            const float p[8][3]{{ax, ay, za - 0.2f}, {ax + ox, ay + oy, za - 0.2f}, {bx + ox, by + oy, zb - 0.2f}, {bx, by, zb - 0.2f},
                                {ax, ay, za + 1.0f}, {ax + ox, ay + oy, za + 1.0f}, {bx + ox, by + oy, zb + 1.0f}, {bx, by, zb + 1.0f}};
            o.mat("dunkel");
            o.hexa(p);
        }
        {
            // the landing in the corner the flight arrives at
            const auto &g = fl[(k + 1) % 4];
            const float zl = z0 + rise * (float)steps;
            const float x0 = g.x + (float)g.wx * w, y0 = g.y + (float)g.wy * w;
            const float x1 = g.x - (float)g.dx * w, y1 = g.y - (float)g.dy * w;
            o.mat("holz");
            o.box(juce::jmin(x0, x1), juce::jmin(y0, y1), zl - 0.2f, juce::jmax(x0, x1), juce::jmax(y0, y1), zl);
        }
    }
    // the top landing's rail (towards the well of the stair)
    o.mat("dunkel");
    o.box(w, S - w, 20.0f, w + 0.08f, S, 21.0f);
    return o.text();
}
} // namespace

const std::vector<BuiltInRoom> &builtInRooms()
{
    static const std::vector<BuiltInRoom> rooms{
        {"rysum_kapelle.obj", "Kapelle", "12 x 24 x 9 m: S\xc3\xa4ulen, B\xc3\xa4nke, Altar", 0.5f, 0.11f, makeKapelle},
        {"rysum_hallenkirche.obj", "Hallenkirche", "21 x 40 x 15 m: drei Schiffe, Empore mit Treppe", 0.5f, 0.27f, makeHallenkirche},
        {"rysum_krypta.obj", "Krypta", "13 x 17 x 4 m: niedrig, sechzehn S\xc3\xa4ulen", 0.5f, 0.31f, makeKrypta},
        {"rysum_rotunde.obj", "Rotunde", "18 m rund, 15 m hoch: S\xc3\xa4ulenring und Kuppel", 0.5f, 0.5f, makeRotunde},
        {"rysum_kreuzgang.obj", "Kreuzgang", "24 x 24 m: Umgang um einen Hof mit Brunnen", 0.5f, 0.5f, makeKreuzgang},
        {"rysum_treppenturm.obj", "Treppenturm", "9 x 9 x 23 m: hundert Stufen nach oben", 0.5f, 0.5f, makeTreppenturm}};
    return rooms;
}

juce::String builtInRoomMaterials()
{
    return "# Rysum: the colours of the built-in room models (seen as greys)\n"
           "newmtl stein\nKd 0.62 0.60 0.56\n"
           "newmtl hell\nKd 0.80 0.78 0.74\n"
           "newmtl dunkel\nKd 0.34 0.33 0.31\n"
           "newmtl holz\nKd 0.42 0.36 0.28\n"
           "newmtl boden\nKd 0.50 0.49 0.46\n";
}

int builtInRoomIndex(const juce::String &fileName)
{
    const auto &rooms = builtInRooms();
    for (int i = 0; i < (int)rooms.size(); ++i)
        if (fileName.equalsIgnoreCase(rooms[(size_t)i].file))
            return i;
    return -1;
}

juce::File writeBuiltInRoom(int index, const juce::File &folder)
{
    const auto &rooms = builtInRooms();
    if (index < 0 || index >= (int)rooms.size())
        return {};
    folder.createDirectory();
    auto put = [&](const juce::String &name, const juce::String &text)
    {
        const auto f = folder.getChildFile(name);
        if (!f.existsAsFile() || f.loadFileAsString() != text)
            f.replaceWithText(text, false, false, "\n");
        return f;
    };
    put(builtInRoomMaterialFile(), builtInRoomMaterials());
    return put(rooms[(size_t)index].file, rooms[(size_t)index].make());
}
} // namespace rysum
