#include "WalkGpu.h"

namespace rysum
{
using namespace juce::gl;

namespace
{
// the triangles arrive projected: x, y on the screen and 1 / depth. Put back into a position with the depth as
// w, so that the card interpolates the texture coordinates in perspective (and brightness, emission and 1 / depth
// along the screen, as Raster does).
const char* const kVertex = R"(#version 150
in vec3 aPos;
in vec4 aAttr;
in vec2 aMat;
noperspective out float vB;
noperspective out float vE;
noperspective out float vIz;
smooth out vec2 vUV;
flat out float vTone;
flat out float vSlot;
void main()
{
    float zv = 1.0 / aPos.z;
    float zn = 1.0 - 2.0 * aPos.z * 0.06;      // (0.06: the near plane; nearer = smaller)
    gl_Position = vec4 (aPos.x * zv, aPos.y * zv, zn * zv, zv);
    vB = aAttr.x; vUV = aAttr.yz; vE = aAttr.w; vIz = aPos.z;
    vTone = aMat.x; vSlot = aMat.y;
}
)";

// Raster::texture and Raster::shadeBand, line for line
const char* const kFragment = R"(#version 150
noperspective in float vB;
noperspective in float vE;
noperspective in float vIz;
smooth in vec2 vUV;
flat in float vTone;
flat in float vSlot;
uniform vec4 uMat[32];      // kind, tone, roughness
uniform vec4 uFog;          // fog tone, 1 / fog distance, grain
uniform vec2 uView;         // the picture's size in pixels
uniform ivec2 uFrame;       // the grain's offset of this frame
out vec4 colour;

float hash2 (int x, int y)
{
    uint h = uint (x) * 374761393u + uint (y) * 668265263u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return float ((h ^ (h >> 16u)) & 0xffffffu) / 16777216.0;
}
float vnoise (float u, float v)
{
    float fu = floor (u), fv = floor (v);
    int iu = int (fu) & 255, iv = int (fv) & 255, iu1 = (iu + 1) & 255, iv1 = (iv + 1) & 255;
    float a = u - fu, b = v - fv;
    a = a * a * (3.0 - 2.0 * a); b = b * b * (3.0 - 2.0 * b);
    float h00 = hash2 (iu, iv), h10 = hash2 (iu1, iv), h01 = hash2 (iu, iv1), h11 = hash2 (iu1, iv1);
    float top = h00 + (h10 - h00) * a;
    return top + ((h01 + (h11 - h01) * a) - top) * b;
}
float pattern (int kind, float r, float u, float v)
{
    if (kind == 1)          // stone
    {
        float row = floor (v / 0.32);
        float uu = u + float (int (row) & 1) * 0.31;
        float col = floor (uu / 0.62);
        float fu = uu - col * 0.62, fv = v - row * 0.32;
        if (fu < 0.018 || fv < 0.018) return 0.5;
        return (0.84 + 0.32 * hash2 (int (col), int (row))) * (1.0 + r * 1.4 * (vnoise (u * 7.0, v * 7.0) - 0.5));
    }
    if (kind == 2)          // plaster
        return 1.0 + r * 1.3 * (vnoise (u * 0.7, v * 0.7) - 0.5) + 0.35 * r * (vnoise (u * 11.0, v * 11.0) - 0.5);
    if (kind == 3)          // wood
    {
        float col = floor (u / 0.2);
        float fu = u - col * 0.2;
        if (fu < 0.008) return 0.42;
        float grain = 0.9 + 0.1 * sin (u * 95.0 + 7.0 * vnoise (col * 3.1 + 0.3, v * 1.4));
        return (0.8 + 0.3 * hash2 (int (col), 17)) * grain * (1.0 + r * (vnoise (u * 20.0, v * 3.0) - 0.5));
    }
    if (kind == 4)          // glass
    {
        float fu = fract (u / 0.45) * 0.45, fv = fract (v / 0.7) * 0.7;
        if (fu < 0.022 || fv < 0.022) return 0.25;
        return 1.12 + 0.16 * sin (v * 1.3 + u * 0.7) + 0.1 * r * (vnoise (u * 4.0, v * 4.0) - 0.5);
    }
    if (kind == 5)          // curtain
    {
        float f = 0.5 + 0.5 * cos (u * 6.2831853 / 0.32 + 0.6 * sin (v * 0.7));
        return (0.5 + 0.55 * (f * sqrt (f))) * (1.0 + 0.5 * r * (vnoise (u * 30.0, v * 30.0) - 0.5));
    }
    if (kind == 6)          // mirror
    {
        float fu = fract (u / 1.2) * 1.2;
        if (fu < 0.03) return 0.55;
        return 1.22 + 0.22 * sin (v * 0.55 + 0.4);
    }
    if (kind == 7)          // slabs
    {
        float cu = floor (u / 0.8), cv = floor (v / 0.8);
        float fu = u - cu * 0.8, fv = v - cv * 0.8;
        if (fu < 0.012 || fv < 0.012) return 0.55;
        return (0.78 + 0.4 * hash2 (int (cu), int (cv))) * (1.0 + r * 1.2 * (vnoise (u * 5.0, v * 5.0) - 0.5));
    }
    if (kind == 8)          // boards
    {
        float row = floor (v / 0.16);
        float fv = v - row * 0.16;
        float uu = u + hash2 (int (row), 5) * 2.4;
        float fu = fract (uu / 2.4) * 2.4;
        if (fv < 0.006 || fu < 0.008) return 0.45;
        float grain = 0.9 + 0.1 * sin (v * 120.0 + 6.0 * vnoise (uu * 1.2, row * 2.7));
        return (0.82 + 0.3 * hash2 (int (row), int (floor (uu / 2.4)))) * grain;
    }
    if (kind == 9)          // carpet
        return 0.9 + 0.2 * (vnoise (u * 40.0, v * 40.0) - 0.5) + 0.12 * (vnoise (u * 2.0, v * 2.0) - 0.5);
    if (kind == 10)         // metal: a pipe
    {
        float uw = abs (fract (u + 0.5) - 0.5);
        if (uw < 0.075 && v > 0.175 && v < 0.215) return 0.1;
        if (uw < 0.085 && v >= 0.215 && v < 0.228) return 0.6;
        float foot = v < 0.16 ? 0.88 : 1.0;
        return foot * (1.0 + 0.07 * (vnoise (u * 40.0, v * 70.0) - 0.5));
    }
    if (kind == 11) return 0.12;        // dark
    return 1.0 + 0.1 * r * (vnoise (u * 3.0, v * 3.0) - 0.5);
}
void main()
{
    float x = gl_FragCoord.x, y = uView.y - gl_FragCoord.y;      // (rows from the top, as Raster counts them)
    float l;
    if (vSlot < -0.5)
        l = uFog.x * (0.55 + 0.45 * (1.0 - abs (floor (y) / uView.y - 0.5) * 2.0));     // the dark vault
    else
    {
        vec4 m = uMat[int (vSlot + 0.5)];
        l = m.y * vTone * vB * pattern (int (m.x + 0.5), m.z, vUV.x, vUV.y) + vE;
        float d = 1.0 / vIz;
        float f = d * uFog.y / (1.0 + d * uFog.y);
        l += (uFog.x - l) * f;
    }
    float cx = 0.5 * uView.x, cy = 0.5 * uView.y;
    float dx = floor (x) - cx, dy = floor (y) - cy;
    l *= 1.0 - 0.38 * (dx * dx + dy * dy) / (cx * cx + cy * cy);
    float gr = hash2 (int (x) + uFrame.x, int (y) * 3 + uFrame.y) - 0.5;
    l += uFog.z * gr * (0.3 + 0.6 * min (1.0, l * 2.0)) * (1.0 - 0.55 * min (1.0, l));
    l = clamp (l, 0.0, 1.0);
    float g2 = l * l * (3.0 - 2.0 * l) * 0.35 + l * 0.65;
    vec3 c = vec3 (13.0 + 222.0 * g2 + 6.0 * l * (1.0 - l), 13.0 + 219.0 * g2 + 2.0 * l * (1.0 - l), 12.0 + 214.0 * g2 - 5.0 * l * (1.0 - l));
    colour = vec4 (floor (clamp (c, 0.0, 255.0)) / 255.0, 1.0);
}
)";
// what lies over the picture: a glow (the same soft light as the sprite the processor's picture uses), or a line
const char* const kOverVertex = R"(#version 150
in vec2 aPos;
in vec3 aUv;
out vec3 vUv;
void main() { gl_Position = vec4 (aPos, 0.0, 1.0); vUv = aUv; }
)";
const char* const kOverFragment = R"(#version 150
in vec3 vUv;
uniform int uMode;
out vec4 colour;
void main()
{
    if (uMode == 1) { colour = vec4 (0.659, 0.267, 0.180, 0.55); return; }      // the rubric red
    float d = length (vUv.xy);
    if (d >= 1.0) discard;
    vec4 c0 = vec4 (0.965, 0.957, 0.941, 0.5), c1 = vec4 (0.863, 0.855, 0.831, 0.2), c2 = vec4 (0.725, 0.718, 0.698, 0.06), c3 = vec4 (0.914, 0.906, 0.886, 0.0);
    vec4 c = d < 0.3 ? mix (c0, c1, d / 0.3) : (d < 0.65 ? mix (c1, c2, (d - 0.3) / 0.35) : mix (c2, c3, (d - 0.65) / 0.35));
    colour = vec4 (c.rgb, c.a * vUv.z);
}
)";
} // namespace

void GpuFrame::addGlow (float x, float y, float rx, float ry, float strength)
{
    const float q[6][2] { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    for (auto& k : q)
        glows.insert (glows.end(), { x + k[0] * rx, y + k[1] * ry, k[0], k[1], strength });
}

GpuPicture::GpuPicture()
{
    juce::OpenGLPixelFormat pf;
    pf.depthBufferBits = 24;
    context.setPixelFormat (pf);
    context.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);
    context.setRenderer (this);
    context.setComponentPaintingEnabled (true);
    context.setContinuousRepainting (false);
}

GpuPicture::~GpuPicture() { detach(); }

void GpuPicture::attachTo (juce::Component& c)
{
    if (! context.isAttached()) context.attachTo (c);
}

void GpuPicture::detach()
{
    if (context.isAttached()) context.detach();
    ready = false;
}

juce::String GpuPicture::failure() const { const juce::ScopedLock l (lock); return failText; }
juce::String GpuPicture::cardName() const { const juce::ScopedLock l (lock); return card; }

void GpuPicture::submit (GpuFrame& frame)
{
    {
        const juce::ScopedLock l (lock);
        std::swap (pending, frame);
        havePending = true;
    }
    context.triggerRepaint();
}

void GpuPicture::newOpenGLContextCreated()
{
    auto fail = [this] (const juce::String& why)
    {
        const juce::ScopedLock l (lock);
        failText = why;
        failed = true;
    };
    program = std::make_unique<juce::OpenGLShaderProgram> (context);
    // (test, RYSUM_GPU_BREAK: a shader that does not compile - the view must go back to the processor's picture)
    const bool broken = juce::SystemStats::getEnvironmentVariable ("RYSUM_GPU_BREAK", {}).isNotEmpty();
    if (! (program->addVertexShader (kVertex) && program->addFragmentShader (broken ? juce::String (kFragment) + "\nthis is not a shader\n" : juce::String (kFragment))))
    {
        fail ("Shader: " + program->getLastError());
        program.reset();
        return;
    }
    const auto id = program->getProgramID();
    glBindAttribLocation (id, 0, "aPos");
    glBindAttribLocation (id, 1, "aAttr");
    glBindAttribLocation (id, 2, "aMat");
    if (! program->link())
    {
        fail ("Shader: " + program->getLastError());
        program.reset();
        return;
    }
    uMat = glGetUniformLocation (id, "uMat");
    uFog = glGetUniformLocation (id, "uFog");
    uView = glGetUniformLocation (id, "uView");
    uFrame = glGetUniformLocation (id, "uFrame");
    glGenVertexArrays (1, &vao);
    glGenBuffers (1, &vbo);
    // (the glows and lines: without them the picture is still whole, so a failure here is no reason to give up)
    overProgram = std::make_unique<juce::OpenGLShaderProgram> (context);
    if (overProgram->addVertexShader (kOverVertex) && overProgram->addFragmentShader (kOverFragment))
    {
        glBindAttribLocation (overProgram->getProgramID(), 0, "aPos");
        glBindAttribLocation (overProgram->getProgramID(), 1, "aUv");
    }
    if (overProgram->link())
    {
        uOverMode = glGetUniformLocation (overProgram->getProgramID(), "uMode");
        glGenVertexArrays (1, &overVao);
        glGenBuffers (1, &overVbo);
    }
    else
        overProgram.reset();
    {
        const juce::ScopedLock l (lock);
        const auto* name = (const char*) glGetString (GL_RENDERER);
        card = name != nullptr ? juce::String::fromUTF8 (name) : juce::String();
    }
    ready = true;
}

void GpuPicture::openGLContextClosing()
{
    ready = false;
    if (vbo != 0) glDeleteBuffers (1, &vbo);
    if (vao != 0) glDeleteVertexArrays (1, &vao);
    if (overVbo != 0) glDeleteBuffers (1, &overVbo);
    if (overVao != 0) glDeleteVertexArrays (1, &overVao);
    vbo = vao = overVbo = overVao = 0;
    program.reset();
    overProgram.reset();
}

void GpuPicture::renderOpenGL()
{
    if (program == nullptr || vao == 0) return;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    bool fresh = false;
    {
        const juce::ScopedLock l (lock);
        if (havePending)
        {
            std::swap (drawing, pending);
            havePending = false;
            fresh = true;
        }
    }
    GLint vp[4] { 0, 0, 1, 1 };
    glGetIntegerv (GL_VIEWPORT, vp);
    const int w = juce::jmax (1, vp[2]), h = juce::jmax (1, vp[3]);

    glDisable (GL_BLEND);
    glDisable (GL_CULL_FACE);
    glDisable (GL_SCISSOR_TEST);
    glDepthMask (GL_TRUE);
    glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth (1.0);
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    program->use();
    glUniform4fv (uMat, 32, drawing.mats.data());
    glUniform4f (uFog, drawing.fogTone, drawing.fogInv, drawing.grain, 0.0f);
    glUniform2f (uView, (float) w, (float) h);
    glUniform2i (uFrame, (GLint) (drawing.frameNo * 7919u), (GLint) (drawing.frameNo * 104729u));

    glBindVertexArray (vao);
    glBindBuffer (GL_ARRAY_BUFFER, vbo);
    // the vault behind everything (two triangles, slot -1), then the scene
    static const float sky[] { -1, -1, 1, 0, 0, 0, 0, 1, -1,   1, -1, 1, 0, 0, 0, 0, 1, -1,   1, 1, 1, 0, 0, 0, 0, 1, -1,
                               -1, -1, 1, 0, 0, 0, 0, 1, -1,   1, 1, 1, 0, 0, 0, 0, 1, -1,   -1, 1, 1, 0, 0, 0, 0, 1, -1 };
    const GLsizei stride = (GLsizei) (kFloats * sizeof (float));
    auto layout = [stride]
    {
        glEnableVertexAttribArray (0);
        glEnableVertexAttribArray (1);
        glEnableVertexAttribArray (2);
        glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, stride, (const void*) 0);
        glVertexAttribPointer (1, 4, GL_FLOAT, GL_FALSE, stride, (const void*) (3 * sizeof (float)));
        glVertexAttribPointer (2, 2, GL_FLOAT, GL_FALSE, stride, (const void*) (7 * sizeof (float)));
    };
    const size_t skyFloats = sizeof (sky) / sizeof (float);
    juce::ignoreUnused (fresh);
    // (one buffer: the vault first, the scene after it)
    glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) ((skyFloats + drawing.verts.size()) * sizeof (float)), nullptr, GL_STREAM_DRAW);
    glBufferSubData (GL_ARRAY_BUFFER, 0, (GLsizeiptr) sizeof (sky), sky);
    if (! drawing.verts.empty())
        glBufferSubData (GL_ARRAY_BUFFER, (GLintptr) sizeof (sky), (GLsizeiptr) (drawing.verts.size() * sizeof (float)), drawing.verts.data());
    layout();
    glDisable (GL_DEPTH_TEST);
    glDepthMask (GL_FALSE);
    glDrawArrays (GL_TRIANGLES, 0, 6);
    glEnable (GL_DEPTH_TEST);
    glDepthFunc (GL_LESS);
    glDepthMask (GL_TRUE);
    if (! drawing.verts.empty())
        glDrawArrays (GL_TRIANGLES, 6, (GLsizei) (drawing.verts.size() / (size_t) kFloats));

    if (drawing.compareWith.isValid())
    {
        compare (drawing, w, h);
        drawing.compareWith = {};
    }
    glDisableVertexAttribArray (2);
    drawOver();

    // (as JUCE's own drawing expects it)
    glDisable (GL_DEPTH_TEST);
    glDisableVertexAttribArray (0);
    glDisableVertexAttribArray (1);
    glDisableVertexAttribArray (2);
    glBindBuffer (GL_ARRAY_BUFFER, 0);
    glBindVertexArray (0);
    glUseProgram (0);
    lastMs = (float) (juce::Time::getMillisecondCounterHiRes() - t0);
    ++fpsCount;
    if (t0 - fpsSince >= 1000.0)
    {
        fps = fpsSince > 0.0 ? (float) (1000.0 * fpsCount / (t0 - fpsSince)) : 0.0f;
        fpsSince = t0; fpsCount = 0;
    }
}

void GpuPicture::drawOver()
{
    if (overProgram == nullptr || overVao == 0 || (drawing.glows.empty() && drawing.lines.empty())) return;
    glDisable (GL_DEPTH_TEST);
    glDepthMask (GL_FALSE);
    glEnable (GL_BLEND);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    overProgram->use();
    glBindVertexArray (overVao);
    glBindBuffer (GL_ARRAY_BUFFER, overVbo);
    glEnableVertexAttribArray (0);
    if (! drawing.lines.empty())
    {
        glUniform1i (uOverMode, 1);
        glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (drawing.lines.size() * sizeof (float)), drawing.lines.data(), GL_STREAM_DRAW);
        glDisableVertexAttribArray (1);
        glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, (GLsizei) (2 * sizeof (float)), (const void*) 0);
        glDrawArrays (GL_LINES, 0, (GLsizei) (drawing.lines.size() / 2));
    }
    if (! drawing.glows.empty())
    {
        glUniform1i (uOverMode, 0);
        glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (drawing.glows.size() * sizeof (float)), drawing.glows.data(), GL_STREAM_DRAW);
        glEnableVertexAttribArray (1);
        glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, (GLsizei) (5 * sizeof (float)), (const void*) 0);
        glVertexAttribPointer (1, 3, GL_FLOAT, GL_FALSE, (GLsizei) (5 * sizeof (float)), (const void*) (2 * sizeof (float)));
        glDrawArrays (GL_TRIANGLES, 0, (GLsizei) (drawing.glows.size() / 5));
    }
    glDisable (GL_BLEND);
    glDepthMask (GL_TRUE);
}

/** test: the card's picture read back, saved beside the processor's picture of the same frame, and how far
    they are apart (grey steps of 255) */
void GpuPicture::compare (const GpuFrame& f, int w, int h)
{
    glFinish();
    std::vector<uint8_t> px ((size_t) w * (size_t) h * 4);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    juce::Image got (juce::Image::ARGB, w, h, false);
    {
        juce::Image::BitmapData bd (got, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < h; ++y)
        {
            const uint8_t* src = px.data() + (size_t) (h - 1 - y) * (size_t) w * 4;
            auto* dst = (uint32_t*) bd.getLinePointer (y);
            for (int x = 0; x < w; ++x)
                dst[x] = 0xff000000u | ((uint32_t) src[4 * x] << 16) | ((uint32_t) src[4 * x + 1] << 8) | (uint32_t) src[4 * x + 2];
        }
    }
    f.compareDir.createDirectory();
    auto save = [&] (const juce::Image& im, const char* name)
    {
        auto file = f.compareDir.getChildFile (name);
        file.deleteFile();
        juce::FileOutputStream os (file);
        if (os.openedOk()) juce::PNGImageFormat().writeImageToStream (im, os);
    };
    save (got, "gpu.png");
    save (f.compareWith, "cpu.png");
    juce::String text;
    text << "card: " << cardName() << "\n" << "picture " << w << " x " << h << ", the processor's " << f.compareWith.getWidth() << " x " << f.compareWith.getHeight() << "\n";
    if (f.compareWith.getWidth() == w && f.compareWith.getHeight() == h)
    {
        juce::Image::BitmapData a (got, juce::Image::BitmapData::readOnly), b (f.compareWith, juce::Image::BitmapData::readOnly);
        double sum = 0.0;
        int worst = 0;
        int64_t over4 = 0, over16 = 0;
        juce::Image diff (juce::Image::ARGB, w, h, false);
        juce::Image::BitmapData dd (diff, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < h; ++y)
        {
            const auto* pa = (const uint32_t*) a.getLinePointer (y);
            const auto* pb = (const uint32_t*) b.getLinePointer (y);
            auto* pd = (uint32_t*) dd.getLinePointer (y);
            for (int x = 0; x < w; ++x)
            {
                const int d = std::abs ((int) ((pa[x] >> 8) & 255u) - (int) ((pb[x] >> 8) & 255u));
                sum += d; worst = juce::jmax (worst, d);
                over4 += d > 4 ? 1 : 0; over16 += d > 16 ? 1 : 0;
                const uint32_t v = (uint32_t) juce::jmin (255, d * 8);
                pd[x] = 0xff000000u | (v << 16) | (v << 8) | v;
            }
        }
        const double n = (double) w * (double) h;
        text << "mean difference " << juce::String (sum / n, 3) << " of 255, worst " << worst << "\n"
             << "pixels more than 4 apart: " << juce::String (100.0 * (double) over4 / n, 3) << " %, more than 16: " << juce::String (100.0 * (double) over16 / n, 3) << " %\n";
        save (diff, "difference_x8.png");
    }
    else
        text << "sizes differ: not compared\n";
    f.compareDir.getChildFile ("result.txt").replaceWithText (text);
    ++compared;
}
} // namespace rysum
