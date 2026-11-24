#pragma once

#include "Walk3D.h"

namespace rysum
{
/** The 360° frames drawn by the graphics card. The recorder's drawing thread owns it: it opens an OpenGL 3.2
    context of its own there (a hidden window - the 3D view may be closed long before the frames are drawn),
    and every call is made on that thread.

    Per face of the cube the processor still prepares the triangles (Raster: projected, clipped, lit), as for
    the view's own picture on the card; the card fills the face's pixels with the same shader as the view's
    (WalkGpu), in its panorama variant (no vignette, the fog by the ray's true length, the vault by the ray's
    elevation), into one layer of a texture array. A second pass resamples the six faces into the
    equirectangular picture - the processor's formula, bilinear on the faces' own pixels - the glows of the
    sounding pipes are laid over it, and it is read back.
    Windows only for now; elsewhere open() says no and the processor draws. */
class PanoGpu
{
public:
    PanoGpu();
    ~PanoGpu();

    /** the context, the shaders; false (and why) if the card cannot */
    bool open (juce::String& why);
    bool isOpen() const noexcept { return ok; }
    juce::String cardName() const { return card; }
    /** the largest texture the card takes (a face and the panorama must fit) */
    int maxTexture() const noexcept { return maxTex; }

    /** the faces' size; (re)made when it changes */
    bool beginFrame (int faceSize, int width, juce::String& why);
    /** one face: the triangles the Raster has prepared for it (Raster::exportGpu), its light and fog */
    struct FaceLook
    {
        float fogTone, fogInv, grain;
        uint32_t frameNo;
        bool dark;
        float ambient, focalPerWidth;
        const std::vector<float>* lamps;  // (Raster::lampsInView)
        V3 rightUpFwdZ;                   // the z of the face's right, up, forward: the vault's elevation
    };
    bool drawFace (int face, const std::vector<float>& verts, const std::array<float, 32 * 4>& mats, const FaceLook& look, juce::String& why);
    /** after drawFace: the distance stored at a pixel of that face (rows from the top; 0 = nothing there), as
        Raster::depthAt gives it */
    float depthAt (int x, int y);
    /** a glow of a sounding pipe over the panorama: its middle and radii in pixels, its strength */
    struct Glow { float cx, cy, rw, rh, alpha; };
    /** the six faces into the panorama: per face its forward and right (the up = right x forward), the focal
        length in face pixels; 'out' is an RGB image width x width / 2 */
    bool resample (const std::array<V3, 6>& fwd, const std::array<V3, 6>& right, float focal, const std::vector<Glow>& glows,
                   juce::Image& out, juce::String& why);
    void close();
    /** (test) the card fails at the fourth face of the n-th frame drawn, as a driver might on the way */
    void failAtFrameForTest (int n) noexcept { failAt = n; }

private:
    struct Native;
    std::unique_ptr<Native> native;
    bool ok = false;
    juce::String card;
    int maxTex = 0;
    unsigned int faceProg = 0, sampleProg = 0, glowProg = 0, glowVbo = 0, vao = 0, vbo = 0, faceTex = 0, faceDepth = 0, faceFbo = 0, outTex = 0, outFbo = 0;
    int F = 0, W = 0, failAt = 0, framesBegun = 0;
    int uMat = -1, uFog = -1, uView = -1, uFrame = -1, uDark = -1, uLamp = -1, uLampR = -1, uLampN = -1, uPano = -1;
    int gSize = -1, sFaces = -1, sF = -1, sFocal = -1, sSize = -1, sFwd = -1, sRight = -1;
    void releaseTargets();
};
} // namespace rysum
