#pragma once

#include <juce_graphics/juce_graphics.h>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>

namespace rysum
{
/** a point in the room: x across (right), y along (front = +y, "vorn"), z up; metres */
struct V3 { float x = 0, y = 0, z = 0; };
inline V3 operator+ (V3 a, V3 b) noexcept { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator- (V3 a, V3 b) noexcept { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator* (V3 a, float s) noexcept { return { a.x * s, a.y * s, a.z * s }; }
inline float dot (V3 a, V3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross (V3 a, V3 b) noexcept { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline V3 normalised (V3 a) noexcept { const float l = std::sqrt (dot (a, a)); return l > 1.0e-12f ? a * (1.0f / l) : V3 { 0, 0, 1 }; }

/** one triangle with its own colour (a loaded model) */
struct Tri { V3 a, b, c; juce::Colour colour; };

/** a loaded room model (an OBJ file), already in room coordinates: its floor at z = 0, its corner at x = y = 0 */
struct RoomMesh
{
    std::vector<Tri> tris;
    V3 size;                          // bounding box (m)
    juce::String name, message;       // file name; what happened while loading (too big, ...)
    int dropped = 0;                  // triangles left out (over the limit)
};

/** OBJ import: v, f (v, v/vt, v//vn, v/vt/vn, negative indices, n-gons), mtllib / usemtl (Kd colours).
    zUp = the file has z up (Blender export without "Y up", SketchUp, ...); otherwise y up, -z forward.
    scale: file units -> metres. Returns null if nothing could be read (error in 'error'). */
std::shared_ptr<RoomMesh> loadObjRoom (const juce::File& file, bool zUp, float scale, juce::String& error);

/** a small chapel as OBJ text (z up), for the tests */
juce::String makeTestObj();

/** how a surface looks: drawn procedurally, in greys (like an old print) */
struct Material
{
    enum Kind : uint8_t { plain = 0, stone, plaster, wood, glass, curtain, mirror, slabs, boards, carpet, metal, dark, kNumKinds };
    Kind kind = plain;
    float tone = 0.7f;        // brightness 0..1
    float rough = 0.1f;       // how much the surface varies (scatter)
    float gloss = 0.1f;       // highlights (hard, smooth surfaces)
};

/** the surfaces of the Saal from its acoustic parameters: what the walls are made of (the nearest of the
    Saal page's materials) and how bright, rough and glossy they look. Floor and ceiling from their absorption. */
Material wallMaterial (float absWalls, float highDamp, float scatter);
Material floorMaterial (float absFloor, float highDamp, float scatter, bool ceiling);
/** the name of what the walls look like (for the display) */
juce::String materialName (Material::Kind k);

/** the renderer: triangles are collected (transformed, clipped, binned into bands of rows), then finish()
    rasterises the bands on several threads (depth first, then every visible pixel shaded once), textures,
    fog, film grain and a vignette; the result in greys. */
class Raster
{
public:
    Raster();
    ~Raster();

    void setSize (int w, int h);
    int width() const noexcept { return w; }
    int height() const noexcept { return h; }

    /** eye position, yaw (degrees, positive = turned to the left, like the azimuth), pitch (degrees, up),
        horizontal field of view (degrees) */
    void setCamera (V3 eye, float yawDeg, float pitchDeg, float fovDeg);
    /** starts a frame: fog tone (0..1) and how far until it is half fog (m); grain amount (0..1), frame number */
    void begin (float fogTone, float fogDistance, float grain, uint32_t frame);

    /** a material slot (0..31) used by the triangles below */
    void setMaterial (int slot, const Material& m);

    struct Vert { V3 p; V3 n; float u = 0, v = 0; };
    /** a triangle; n = vertex normals (smooth) or all the face normal. tone multiplies the material's;
        emissive brightens (a sounding pipe). */
    void triangle (const Vert& a, const Vert& b, const Vert& c, int slot, float tone = 1.0f, float emissive = 0.0f);
    /** a flat triangle with texture coordinates taken from the world (by its facing): loaded models */
    void flatTriangle (V3 a, V3 b, V3 c, int slot, float tone, float emissive = 0.0f);

    /** rasterises and shades everything, writes the image */
    void finish();
    /** only the depths (for depthAt): when the graphics card draws the picture */
    void finishDepth();
    /** the frame's triangles for the graphics card - per vertex 9 numbers: x, y (-1 .. 1, y up), 1 / depth,
        brightness, u, v, emission, tone, material slot - and per slot 4: kind, tone, roughness, 0 */
    void exportGpu (std::vector<float>& verts, std::array<float, 32 * 4>& materials) const;
    float fogToneNow() const noexcept { return fogTone; }
    float fogInvNow() const noexcept { return fogInv; }

    /** screen point of a room point (false if behind the near plane) */
    bool project (V3 p, float& sx, float& sy, float& depth) const noexcept;
    /** depth stored at a pixel after finish() (0 = nothing there) */
    float depthAt (int x, int y) const noexcept;
    /** a line in the room, clipped at the near plane: screen points (false if not visible) */
    bool projectLine (V3 a, V3 b, juce::Point<float>& pa, juce::Point<float>& pb) const noexcept;
    /** distance from the eye to a point, along the view (negative = behind) */
    float viewDepth (V3 p) const noexcept;
    V3 eyePos() const noexcept { return eye; }

    const juce::Image& image() const noexcept { return img; }
    int trianglesDrawn = 0;
    float rasterMs = 0.0f, shadeMs = 0.0f;       // the last frame, for tuning
    int workerCount() const noexcept { return (int) workers.size(); }

private:
    struct VV { float x, y, z; float b, u, v, e; };       // view space + attributes (brightness, texture, emission)
    struct ST                                              // a screen triangle, ready to rasterise
    {
        float x[3], y[3], iz[3], b[3], uz[3], vz[3], e[3];
        int minY, maxY;
        uint8_t slot;
        float tone;
    };
    VV toView (V3 p) const noexcept;
    float light (V3 p, V3 n, const Material& m) const noexcept;
    void emitClipped (const VV* v, int n, int slot, float tone);
    void rasterBand (int band);
    void shadeBand (int band);
    void runParallel (int jobs, const std::function<void (int)>& fn);

    int w = 0, h = 0;
    juce::Image img;
    std::vector<uint32_t> pix;
    std::vector<float> zbuf, gu, gv, gb, ge, gt;           // per pixel: 1/depth, texture u/v, brightness, emission, tone
    std::vector<uint8_t> gm;                               // material slot
    std::vector<ST> tris;
    static constexpr int kBandRows = 8;             // (a band and its buffers stay in the cache while it is drawn and shaded)
    std::vector<std::vector<int>> bins;
    std::array<Material, 32> mats {};
    V3 eye, right, up, fwd;
    float focal = 1.0f, fogTone = 0.1f, fogInv = 0.02f, grain = 0.1f;
    uint32_t frameNo = 0;
    bool depthOnly = false;
    V3 lightDir;

    // a few worker threads (the message thread works too)
    std::vector<std::thread> workers;
    std::mutex mx;
    std::condition_variable cvWork, cvDone;
    const std::function<void (int)>* job = nullptr;
    int jobCount = 0, generation = 0, busy = 0;
    std::atomic<int> nextJob { 0 };
    bool quit = false;
    void workerLoop();
};
} // namespace rysum
