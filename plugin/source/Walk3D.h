#pragma once

#include <juce_graphics/juce_graphics.h>
#include <vector>
#include <memory>

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

/** one triangle with its own colour (flat shaded) */
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

/** builds an OBJ text from a mesh-like list of boxes, for the tests */
juce::String makeTestObj();

/** a small software renderer: z-buffered, flat-shaded triangles into an image, with fog */
class Raster
{
public:
    void setSize (int w, int h);
    int width() const noexcept { return w; }
    int height() const noexcept { return h; }

    /** eye position, yaw (degrees, positive = turned to the left, like the azimuth), pitch (degrees, up),
        horizontal field of view (degrees) */
    void setCamera (V3 eye, float yawDeg, float pitchDeg, float fovDeg);
    void clear (juce::Colour sky, juce::Colour fog, float fogDistance);

    /** a triangle seen from both sides (lit by a light from above-front, plus ambient) */
    void drawTri (const Tri& t, float emissive = 0.0f);

    /** a pipe: a vertical cylinder (billboarded) from 'bottom' upwards, with its mouth; depth tested.
        glow 0..1 brightens it (it sounds). */
    void drawPipe (V3 bottom, float length, float radius, juce::Colour metal, float glow);

    /** view space of a point: right, up, depth. False if behind the near plane. */
    bool project (V3 p, float& sx, float& sy, float& depth) const noexcept;
    /** depth stored at a pixel (0 = nothing there) */
    float depthAt (int x, int y) const noexcept;
    /** a line in the room, clipped at the near plane: screen points (false if not visible) */
    bool projectLine (V3 a, V3 b, juce::Point<float>& pa, juce::Point<float>& pb) const noexcept;

    const juce::Image& image() const noexcept { return img; }
    /** copies the pixel buffer into the image (call once after drawing) */
    void finish();

    int trianglesDrawn = 0;

private:
    struct VV { float x, y, z; };          // view space: right, up, depth
    VV toView (V3 p) const noexcept;
    void fillClipped (const VV* v, int n, uint32_t rgb, float bright);
    void rasterTri (VV a, VV b, VV c, float r, float g, float bl);

    int w = 0, h = 0;
    juce::Image img;
    std::vector<uint32_t> pix;
    std::vector<float> zbuf;              // 1 / depth (0 = empty)
    V3 eye, right, up, fwd;
    float focal = 1.0f;
    float fogR = 0, fogG = 0, fogB = 0, fogInv = 0.02f;
    V3 lightDir;
};
} // namespace rysum
