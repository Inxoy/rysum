#include "PanoGpu.h"
#include "WalkGpu.h"
#include <juce_opengl/juce_opengl.h>

namespace rysum
{
using namespace juce::gl;

namespace
{
// the six faces into the equirectangular picture: Pano360's renderPanorama, pixel for pixel. The picture is
// written with its top row at the bottom of the frame buffer, so that reading it back gives the rows in order.
const char* const kSampleVertex = R"(#version 150
void main()
{
    vec2 p = vec2 (float ((gl_VertexID & 1) * 4 - 1), float ((gl_VertexID & 2) * 2 - 1));
    gl_Position = vec4 (p, 0.0, 1.0);
}
)";
const char* const kSampleFragment = R"(#version 150
uniform sampler2DArray uFaces;
uniform int uF;
uniform float uFocal;
uniform vec2 uSize;
uniform vec3 uFwd[6];
uniform vec3 uRight[6];
out vec4 colour;
int faceOf (vec3 d)
{
    vec3 a = abs (d);
    if (a.z >= a.x && a.z >= a.y) return d.z > 0.0 ? 4 : 5;
    if (a.x >= a.y) return d.x > 0.0 ? 1 : 3;
    return d.y > 0.0 ? 0 : 2;
}
vec3 px (int x, int y, int f) { return texelFetch (uFaces, ivec3 (x, uF - 1 - y, f), 0).rgb * 255.0; }
void main()
{
    int i = int (gl_FragCoord.x), j = int (gl_FragCoord.y);
    float lon = ((float (i) + 0.5) / uSize.x - 0.5) * 6.28318530718;
    float lat = (0.5 - (float (j) + 0.5) / uSize.y) * 3.14159265359;
    float cl = cos (lat), sl = sin (lat);
    vec3 r = vec3 (sin (lon) * cl, cos (lon) * cl, sl);
    int f = faceOf (r);
    vec3 fw = uFwd[f], rt = uRight[f], up = cross (rt, fw);
    float vz = dot (r, fw), vx = dot (r, rt), vy = dot (r, up);
    float h = 0.5 * float (uF);
    float x = h + vx / vz * uFocal - 0.5, y = h - vy / vz * uFocal - 0.5;
    int x0 = clamp (int (floor (x)), 0, uF - 2), y0 = clamp (int (floor (y)), 0, uF - 2);
    float fx = clamp (x - float (x0), 0.0, 1.0), fy = clamp (y - float (y0), 0.0, 1.0);
    vec3 a = px (x0, y0, f), b = px (x0 + 1, y0, f), c = px (x0, y0 + 1, f), d = px (x0 + 1, y0 + 1, f);
    vec3 top = a + fx * (b - a), bot = c + fx * (d - c);
    colour = vec4 (floor (clamp (top + fy * (bot - top) + 0.5, 0.0, 255.0)) / 255.0, 1.0);
}
)";

// the glow of a sounding pipe: the same soft light as the processor's sprite (Pano360's glowSprite)
const char* const kGlowVertex = R"(#version 150
in vec2 aPos;
in vec3 aUv;
uniform vec2 uSize;
out vec3 vUv;
void main() { gl_Position = vec4 (aPos.x / uSize.x * 2.0 - 1.0, aPos.y / uSize.y * 2.0 - 1.0, 0.0, 1.0); vUv = aUv; }
)";
const char* const kGlowFragment = R"(#version 150
in vec3 vUv;
out vec4 colour;
void main()
{
    float d = length (vUv.xy);
    if (d >= 1.0) discard;
    vec4 c0 = vec4 (0.965, 0.957, 0.941, 0.5), c1 = vec4 (0.863, 0.855, 0.831, 0.2), c2 = vec4 (0.561, 0.549, 0.525, 0.06), c3 = vec4 (0.0, 0.0, 0.0, 0.0);
    vec4 c = d < 0.3 ? mix (c0, c1, d / 0.3) : (d < 0.65 ? mix (c1, c2, (d - 0.3) / 0.35) : mix (c2, c3, (d - 0.65) / 0.35));
    colour = vec4 (c.rgb, c.a * vUv.z);
}
)";

constexpr GLenum kTex2DArray = 0x8C1A;   // GL_TEXTURE_2D_ARRAY

unsigned int compileProgram (const char* vs, const char* fs, std::initializer_list<const char*> attribs, juce::String& why)
{
    auto stage = [&] (GLenum type, const char* src) -> GLuint
    {
        const GLuint s = glCreateShader (type);
        glShaderSource (s, 1, &src, nullptr);
        glCompileShader (s);
        GLint okc = 0;
        glGetShaderiv (s, GL_COMPILE_STATUS, &okc);
        if (okc == 0)
        {
            char log[2048] {};
            glGetShaderInfoLog (s, (GLsizei) sizeof (log) - 1, nullptr, log);
            why = "Shader: " + juce::String (log);
            glDeleteShader (s);
            return 0;
        }
        return s;
    };
    const GLuint v = stage (GL_VERTEX_SHADER, vs);
    if (v == 0) return 0;
    const GLuint f = stage (GL_FRAGMENT_SHADER, fs);
    if (f == 0) { glDeleteShader (v); return 0; }
    const GLuint p = glCreateProgram();
    glAttachShader (p, v);
    glAttachShader (p, f);
    GLuint loc = 0;
    for (auto* a : attribs) glBindAttribLocation (p, loc++, a);
    glLinkProgram (p);
    glDeleteShader (v);
    glDeleteShader (f);
    GLint linked = 0;
    glGetProgramiv (p, GL_LINK_STATUS, &linked);
    if (linked == 0)
    {
        char log[2048] {};
        glGetProgramInfoLog (p, (GLsizei) sizeof (log) - 1, nullptr, log);
        why = "Shader: " + juce::String (log);
        glDeleteProgram (p);
        return 0;
    }
    return p;
}
} // namespace

// (the context: at the end of the file, with the system's headers)
bool openPanoContext (void*& handle, juce::String& why);
void closePanoContext (void*& handle);
void pumpPanoContext (void* handle);

struct PanoGpu::Native { void* handle = nullptr; };

PanoGpu::PanoGpu() : native (std::make_unique<Native>()) {}
PanoGpu::~PanoGpu() { close(); }

bool PanoGpu::open (juce::String& why)
{
    if (ok) return true;
    if (juce::SystemStats::getEnvironmentVariable ("RYSUM_PANO_CPU", {}).isNotEmpty()) { why = "RYSUM_PANO_CPU"; return false; }
    if (! openPanoContext (native->handle, why)) return false;
    juce::gl::loadFunctions();
    if (glCreateShader == nullptr || glGenFramebuffers == nullptr || glTexImage3D == nullptr || glFramebufferTextureLayer == nullptr)
    {
        why = "OpenGL 3.2 fehlt";
        closePanoContext (native->handle);
        return false;
    }
    {
        const auto* name = (const char*) glGetString (GL_RENDERER);
        card = name != nullptr ? juce::String::fromUTF8 (name) : juce::String();
    }
    GLint mt = 0;
    glGetIntegerv (GL_MAX_TEXTURE_SIZE, &mt);
    maxTex = (int) mt;
    // (RYSUM_GPU_BREAK, as for the view: a shader that does not compile - the processor must take over)
    const bool broken = juce::SystemStats::getEnvironmentVariable ("RYSUM_GPU_BREAK", {}).isNotEmpty();
    const juce::String frag = juce::String (walkFragmentShader()) + (broken ? "\nthis is not a shader\n" : "");
    faceProg = compileProgram (walkVertexShader(), frag.toRawUTF8(), { "aPos", "aAttr", "aMat", "aNrm" }, why);
    if (faceProg != 0) sampleProg = compileProgram (kSampleVertex, kSampleFragment, {}, why);
    if (sampleProg != 0) glowProg = compileProgram (kGlowVertex, kGlowFragment, { "aPos", "aUv" }, why);
    if (faceProg == 0 || sampleProg == 0 || glowProg == 0)
    {
        close();
        return false;
    }
    uMat = glGetUniformLocation (faceProg, "uMat");
    uFog = glGetUniformLocation (faceProg, "uFog");
    uView = glGetUniformLocation (faceProg, "uView");
    uFrame = glGetUniformLocation (faceProg, "uFrame");
    uDark = glGetUniformLocation (faceProg, "uDark");
    uLamp = glGetUniformLocation (faceProg, "uLamp");
    uLampR = glGetUniformLocation (faceProg, "uLampR");
    uLampN = glGetUniformLocation (faceProg, "uLampN");
    uPano = glGetUniformLocation (faceProg, "uPano");
    gSize = glGetUniformLocation (glowProg, "uSize");
    sFaces = glGetUniformLocation (sampleProg, "uFaces");
    sF = glGetUniformLocation (sampleProg, "uF");
    sFocal = glGetUniformLocation (sampleProg, "uFocal");
    sSize = glGetUniformLocation (sampleProg, "uSize");
    sFwd = glGetUniformLocation (sampleProg, "uFwd");
    sRight = glGetUniformLocation (sampleProg, "uRight");
    glGenVertexArrays (1, &vao);
    glGenBuffers (1, &vbo);
    glGenBuffers (1, &glowVbo);
    ok = true;
    return true;
}

void PanoGpu::releaseTargets()
{
    if (faceFbo != 0) glDeleteFramebuffers (1, &faceFbo);
    if (outFbo != 0) glDeleteFramebuffers (1, &outFbo);
    if (faceDepth != 0) glDeleteRenderbuffers (1, &faceDepth);
    if (faceTex != 0) glDeleteTextures (1, &faceTex);
    if (outTex != 0) glDeleteTextures (1, &outTex);
    faceFbo = outFbo = faceDepth = faceTex = outTex = 0;
    F = W = 0;
}

void PanoGpu::close()
{
    if (native->handle != nullptr)
    {
        if (ok || faceProg != 0 || sampleProg != 0)
        {
            releaseTargets();
            if (vbo != 0) glDeleteBuffers (1, &vbo);
            if (glowVbo != 0) glDeleteBuffers (1, &glowVbo);
            if (glowProg != 0) glDeleteProgram (glowProg);
            if (vao != 0) glDeleteVertexArrays (1, &vao);
            if (faceProg != 0) glDeleteProgram (faceProg);
            if (sampleProg != 0) glDeleteProgram (sampleProg);
        }
        closePanoContext (native->handle);
    }
    vbo = vao = faceProg = sampleProg = glowProg = glowVbo = 0;
    ok = false;
}

bool PanoGpu::beginFrame (int faceSize, int width, juce::String& why)
{
    if (! ok) { why = "no context"; return false; }
    pumpPanoContext (native->handle);
    if (faceSize > maxTex || width > maxTex) { why = "picture larger than the card's textures (" + juce::String (maxTex) + ")"; return false; }
    if (faceSize == F && width == W) return true;
    releaseTargets();
    F = faceSize;
    W = width;
    const int H = W / 2;
    glGenTextures (1, &faceTex);
    glBindTexture (kTex2DArray, faceTex);
    glTexParameteri (kTex2DArray, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (kTex2DArray, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri (kTex2DArray, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (kTex2DArray, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage3D (kTex2DArray, 0, GL_RGBA8, F, F, 6, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenRenderbuffers (1, &faceDepth);
    glBindRenderbuffer (GL_RENDERBUFFER, faceDepth);
    glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, F, F);
    glGenFramebuffers (1, &faceFbo);
    glBindFramebuffer (GL_FRAMEBUFFER, faceFbo);
    glFramebufferTextureLayer (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, faceTex, 0, 0);
    glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, faceDepth);
    const GLenum s1 = glCheckFramebufferStatus (GL_FRAMEBUFFER);
    glGenTextures (1, &outTex);
    glBindTexture (GL_TEXTURE_2D, outTex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers (1, &outFbo);
    glBindFramebuffer (GL_FRAMEBUFFER, outFbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outTex, 0);
    const GLenum s2 = glCheckFramebufferStatus (GL_FRAMEBUFFER);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    if (s1 != GL_FRAMEBUFFER_COMPLETE || s2 != GL_FRAMEBUFFER_COMPLETE || glGetError() != GL_NO_ERROR)
    {
        why = "frame buffer " + juce::String::toHexString ((int) s1) + " / " + juce::String::toHexString ((int) s2) + " (" + juce::String (F) + ", " + juce::String (W) + ")";
        releaseTargets();
        return false;
    }
    return true;
}

bool PanoGpu::drawFace (int face, const std::vector<float>& verts, const std::array<float, 32 * 4>& mats, const FaceLook& look, juce::String& why)
{
    if (! ok || faceFbo == 0) { why = "no frame buffer"; return false; }
    glBindFramebuffer (GL_FRAMEBUFFER, faceFbo);
    glFramebufferTextureLayer (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, faceTex, 0, face);
    glViewport (0, 0, F, F);
    glDisable (GL_BLEND);
    glDisable (GL_CULL_FACE);
    glDisable (GL_SCISSOR_TEST);
    glDepthMask (GL_TRUE);
    glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth (1.0);
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram (faceProg);
    glUniform4fv (uMat, 32, mats.data());
    glUniform4f (uFog, look.fogTone, look.fogInv, look.grain, 0.0f);
    glUniform2f (uView, (float) F, (float) F);
    glUniform2i (uFrame, (GLint) (look.frameNo * 7919u), (GLint) (look.frameNo * 104729u));
    glUniform4f (uPano, 1.0f, look.rightUpFwdZ.x, look.rightUpFwdZ.y, look.rightUpFwdZ.z);
    {
        const auto& lamps = *look.lamps;
        const int nl = juce::jmin (24, (int) lamps.size() / 5);
        float lp[24 * 4] {}, lr[24] {};
        for (int i = 0; i < nl; ++i)
        {
            for (int c = 0; c < 4; ++c) lp[4 * i + c] = lamps[(size_t) (5 * i + c)];
            lr[i] = lamps[(size_t) (5 * i + 4)];
        }
        glUniform3f (uDark, look.dark ? 1.0f : 0.0f, look.ambient, look.focalPerWidth);
        glUniform4fv (uLamp, 24, lp);
        glUniform1fv (uLampR, 24, lr);
        glUniform1i (uLampN, nl);
    }
    // the vault behind everything (two triangles, slot -1), then the face's triangles (as GpuPicture draws them)
    static const float sky[] { -1, -1, 1, 0, 0, 0, 0, 1, -1, 0, 0, 1,   1, -1, 1, 0, 0, 0, 0, 1, -1, 0, 0, 1,   1, 1, 1, 0, 0, 0, 0, 1, -1, 0, 0, 1,
                               -1, -1, 1, 0, 0, 0, 0, 1, -1, 0, 0, 1,   1, 1, 1, 0, 0, 0, 0, 1, -1, 0, 0, 1,   -1, 1, 1, 0, 0, 0, 0, 1, -1, 0, 0, 1 };
    constexpr int kFloats = GpuPicture::kFloats;
    const GLsizei stride = (GLsizei) (kFloats * sizeof (float));
    glBindVertexArray (vao);
    glBindBuffer (GL_ARRAY_BUFFER, vbo);
    glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (sizeof (sky) + verts.size() * sizeof (float)), nullptr, GL_STREAM_DRAW);
    glBufferSubData (GL_ARRAY_BUFFER, 0, (GLsizeiptr) sizeof (sky), sky);
    if (! verts.empty())
        glBufferSubData (GL_ARRAY_BUFFER, (GLintptr) sizeof (sky), (GLsizeiptr) (verts.size() * sizeof (float)), verts.data());
    for (GLuint a = 0; a < 4; ++a) glEnableVertexAttribArray (a);
    glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, stride, (const void*) 0);
    glVertexAttribPointer (1, 4, GL_FLOAT, GL_FALSE, stride, (const void*) (3 * sizeof (float)));
    glVertexAttribPointer (2, 2, GL_FLOAT, GL_FALSE, stride, (const void*) (7 * sizeof (float)));
    glVertexAttribPointer (3, 3, GL_FLOAT, GL_FALSE, stride, (const void*) (9 * sizeof (float)));
    glDisable (GL_DEPTH_TEST);
    glDepthMask (GL_FALSE);
    glDrawArrays (GL_TRIANGLES, 0, 6);
    glEnable (GL_DEPTH_TEST);
    glDepthFunc (GL_LESS);
    glDepthMask (GL_TRUE);
    if (! verts.empty())
        glDrawArrays (GL_TRIANGLES, 6, (GLsizei) (verts.size() / (size_t) kFloats));
    glDisable (GL_DEPTH_TEST);
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) { why = "OpenGL error " + juce::String::toHexString ((int) e); return false; }
    return true;
}

float PanoGpu::depthAt (int x, int y)
{
    if (! ok || faceFbo == 0 || x < 0 || y < 0 || x >= F || y >= F) return 0.0f;
    // (the depth the shader stored: 1 - 0.06 / distance; the cleared 1 = nothing there)
    float d = 1.0f;
    glReadPixels (x, F - 1 - y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d);
    const float iz = (1.0f - d) / 0.06f;
    return iz > 1.0e-6f ? 1.0f / iz : 0.0f;
}

bool PanoGpu::resample (const std::array<V3, 6>& fwd, const std::array<V3, 6>& right, float focal, const std::vector<Glow>& glows,
                        juce::Image& out, juce::String& why)
{
    if (! ok || outFbo == 0) { why = "no frame buffer"; return false; }
    const int H = W / 2;
    glBindFramebuffer (GL_FRAMEBUFFER, outFbo);
    glViewport (0, 0, W, H);
    glDisable (GL_DEPTH_TEST);
    glUseProgram (sampleProg);
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (kTex2DArray, faceTex);
    glUniform1i (sFaces, 0);
    glUniform1i (sF, F);
    glUniform1f (sFocal, focal);
    glUniform2f (sSize, (float) W, (float) H);
    float fv[18], rv[18];
    for (int f = 0; f < 6; ++f)
    {
        fv[3 * f] = fwd[(size_t) f].x; fv[3 * f + 1] = fwd[(size_t) f].y; fv[3 * f + 2] = fwd[(size_t) f].z;
        rv[3 * f] = right[(size_t) f].x; rv[3 * f + 1] = right[(size_t) f].y; rv[3 * f + 2] = right[(size_t) f].z;
    }
    glUniform3fv (sFwd, 6, fv);
    glUniform3fv (sRight, 6, rv);
    glBindVertexArray (vao);
    for (GLuint a = 0; a < 4; ++a) glDisableVertexAttribArray (a);
    glDrawArrays (GL_TRIANGLES, 0, 3);

    // the glows: a quad each (and once more across the seam, where one reaches over the picture's edge)
    if (! glows.empty() && glowProg != 0)
    {
        std::vector<float> q;
        q.reserve (glows.size() * 3 * 30);
        const float k[6][2] { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
        for (const auto& g : glows)
            for (float shift : { -(float) W, 0.0f, (float) W })
                if (g.cx + shift + g.rw > 0.0f && g.cx + shift - g.rw < (float) W)
                    for (auto& c : k)
                        q.insert (q.end(), { g.cx + shift + c[0] * g.rw, g.cy + c[1] * g.rh, c[0], c[1], g.alpha });
        glUseProgram (glowProg);
        glUniform2f (gSize, (float) W, (float) H);
        glEnable (GL_BLEND);
        glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindBuffer (GL_ARRAY_BUFFER, glowVbo);
        glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (q.size() * sizeof (float)), q.data(), GL_STREAM_DRAW);
        glEnableVertexAttribArray (0);
        glEnableVertexAttribArray (1);
        glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, (GLsizei) (5 * sizeof (float)), (const void*) 0);
        glVertexAttribPointer (1, 3, GL_FLOAT, GL_FALSE, (GLsizei) (5 * sizeof (float)), (const void*) (2 * sizeof (float)));
        glDrawArrays (GL_TRIANGLES, 0, (GLsizei) (q.size() / 5));
        glDisableVertexAttribArray (0);
        glDisableVertexAttribArray (1);
        glDisable (GL_BLEND);
    }

    // read straight into the picture: as BGRA the bytes are those of JUCE's ARGB pixels (b, g, r, a)
    out = juce::Image (juce::Image::ARGB, W, H, false);
    {
        juce::Image::BitmapData dst (out, juce::Image::BitmapData::writeOnly);
        glPixelStorei (GL_PACK_ALIGNMENT, 4);
        glPixelStorei (GL_PACK_ROW_LENGTH, dst.lineStride / 4);
        glReadPixels (0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, dst.getLinePointer (0));
        glPixelStorei (GL_PACK_ROW_LENGTH, 0);
    }
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) { why = "OpenGL error " + juce::String::toHexString ((int) e); return false; }
    return true;
}
} // namespace rysum

// ---------------------------------------------------------------------------
// the context: a hidden window of the drawing thread's own (Windows)
// ---------------------------------------------------------------------------
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>

namespace
{
struct PanoWgl
{
    HWND wnd = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
};
}

bool rysum::openPanoContext (void*& handle, juce::String& why)
{
    static const wchar_t* const kClass = L"RysumPano360GL";
    WNDCLASSEXW wc {};
    wc.cbSize = sizeof (wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW (nullptr);
    wc.lpszClassName = kClass;
    if (RegisterClassExW (&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { why = "RegisterClass"; return false; }
    auto* w = new PanoWgl();
    auto fail = [&] (const char* what)
    {
        why = what;
        if (w->rc != nullptr) { wglMakeCurrent (nullptr, nullptr); wglDeleteContext (w->rc); }
        if (w->dc != nullptr) ReleaseDC (w->wnd, w->dc);
        if (w->wnd != nullptr) DestroyWindow (w->wnd);
        delete w;
        return false;
    };
    w->wnd = CreateWindowExW (0, kClass, L"Rysum 360", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    if (w->wnd == nullptr) return fail ("CreateWindow");
    w->dc = GetDC (w->wnd);
    PIXELFORMATDESCRIPTOR pfd {};
    pfd.nSize = sizeof (pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int format = ChoosePixelFormat (w->dc, &pfd);
    if (format == 0 || ! SetPixelFormat (w->dc, format, &pfd)) return fail ("no pixel format for OpenGL");
    HGLRC legacy = wglCreateContext (w->dc);
    if (legacy == nullptr || ! wglMakeCurrent (w->dc, legacy)) { if (legacy != nullptr) wglDeleteContext (legacy); return fail ("no OpenGL context"); }
    // a 3.2 core context, as the view asks for (without the extension: the old context, if it does 3.2)
    using CreateAttribs = HGLRC (WINAPI*) (HDC, HGLRC, const int*);
    auto create = (CreateAttribs) (void*) wglGetProcAddress ("wglCreateContextAttribsARB");
    if (create != nullptr)
    {
        const int attribs[] { 0x2091, 3, 0x2092, 2, 0x9126, 0x00000001, 0 };   // major 3, minor 2, core profile
        HGLRC core = create (w->dc, nullptr, attribs);
        if (core != nullptr)
        {
            wglMakeCurrent (nullptr, nullptr);
            wglDeleteContext (legacy);
            legacy = nullptr;
            w->rc = core;
            if (! wglMakeCurrent (w->dc, core)) return fail ("OpenGL 3.2 context not current");
        }
    }
    if (w->rc == nullptr) w->rc = legacy;
    handle = w;
    return true;
}

void rysum::closePanoContext (void*& handle)
{
    auto* w = static_cast<PanoWgl*> (handle);
    if (w == nullptr) return;
    wglMakeCurrent (nullptr, nullptr);
    if (w->rc != nullptr) wglDeleteContext (w->rc);
    if (w->dc != nullptr) ReleaseDC (w->wnd, w->dc);
    if (w->wnd != nullptr) DestroyWindow (w->wnd);
    delete w;
    handle = nullptr;
}

void rysum::pumpPanoContext (void* handle)
{
    auto* w = static_cast<PanoWgl*> (handle);
    if (w == nullptr) return;
    MSG msg;
    while (PeekMessageW (&msg, w->wnd, 0, 0, PM_REMOVE)) DispatchMessageW (&msg);
}
#else
bool rysum::openPanoContext (void*&, juce::String& why) { why = "the graphics card draws the 360 frames on Windows only"; return false; }
void rysum::closePanoContext (void*& handle) { handle = nullptr; }
void rysum::pumpPanoContext (void*) {}
#endif
