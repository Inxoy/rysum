#pragma once

#include "Walk3D.h"
#include <juce_opengl/juce_opengl.h>

namespace rysum
{
/** one frame of the 3D view for the graphics card: the triangles the Raster has prepared (projected, lit,
    clipped), the materials, fog and grain. Built on the message thread. */
struct GpuFrame
{
    std::vector<float> verts;                    // GpuPicture::kFloats per vertex
    std::array<float, 32 * 4> mats {};           // per slot: kind, tone, roughness, 0
    float fogTone = 0.07f, fogInv = 0.02f, grain = 0.12f;
    uint32_t frameNo = 0;
    // "Licht aus": the lamps (Raster::lampsInView: 5 numbers each), what is left of the usual light, and the
    // focal length per pixel of width (to find each pixel's place in the room)
    bool dark = false;
    float ambient = 1.0f, focalPerWidth = 1.0f;
    std::vector<float> lamps;
    // over the picture: the glow of the sounding pipes (per vertex x, y, u, v, strength: six vertices a glow),
    // and lines (the outline of the room behind a model; per vertex x, y)
    std::vector<float> glows, lines;
    void addGlow (float x, float y, float rx, float ry, float strength);
    // a test: the same frame as the processor drew it - the card's picture is read back and compared
    juce::Image compareWith;
    juce::File compareDir;
};

/** The 3D view's picture drawn by the graphics card (OpenGL 3.2). The processor still prepares the triangles;
    the card fills the pixels - the patterns of the surfaces, fog, grain and the grey of the print are the same
    formulas as Raster's, as a shader. The view's writing, glows and controls are drawn over it by JUCE. */
class GpuPicture : private juce::OpenGLRenderer
{
public:
    static constexpr int kFloats = 12;           // x, y (-1 .. 1), 1 / depth, brightness, u, v, emission, tone, slot, normal (3)

    GpuPicture();
    ~GpuPicture() override;

    void attachTo (juce::Component&);
    void detach();
    bool isAttached() const { return context.isAttached(); }
    /** the context exists and the shader compiled: frames are being drawn */
    bool isReady() const noexcept { return ready.load(); }
    /** the card (or its driver) cannot do it: the view goes back to the processor's picture */
    bool hasFailed() const noexcept { return failed.load(); }
    juce::String failure() const;
    /** the card's name (once ready) */
    juce::String cardName() const;
    double renderingScale() const { return context.getRenderingScale(); }

    /** the next frame to draw (message thread); its memory is swapped with the one drawn before */
    void submit (GpuFrame& frame);
    float drawMs() const noexcept { return lastMs.load(); }
    /** pictures the card has drawn per second (measured over the last second) */
    float framesPerSecond() const noexcept { return fps.load(); }
    /** frames whose comparison (GpuFrame::compareWith) has been written */
    int comparisonsDone() const noexcept { return compared.load(); }

private:
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;
    void compare (const GpuFrame&, int w, int h);

    juce::OpenGLContext context;
    std::unique_ptr<juce::OpenGLShaderProgram> program, overProgram;
    unsigned int vao = 0, vbo = 0, overVao = 0, overVbo = 0;
    int uMat = -1, uFog = -1, uView = -1, uFrame = -1, uOverMode = -1, uDark = -1, uLamp = -1, uLampR = -1, uLampN = -1;
    void drawOver();
    juce::CriticalSection lock;
    GpuFrame pending, drawing;                   // (pending: under the lock)
    bool havePending = false;
    std::atomic<bool> ready { false }, failed { false };
    std::atomic<float> lastMs { 0.0f }, fps { 0.0f };
    int fpsCount = 0;
    double fpsSince = 0.0;
    std::atomic<int> compared { 0 };
    juce::String failText, card;                 // (under the lock)
};
} // namespace rysum
