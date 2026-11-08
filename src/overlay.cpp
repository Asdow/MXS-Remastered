// overlay.cpp
//   In-game settings panel ("MXS Remastered"). Config::menuKey (E by default) opens and closes it.
//   Tabs: FIRST PERSON, GRAPHICS, REPLAY (up to three columns of rows each) and KEYBINDS.
//
//   Rendering: the panel is drawn into the game's back buffer right before each swap, as one
//   batch of textured triangles through a tiny shader of our own (GLSL 1.50, 1.30 or 1.20,
//   whichever the context accepts). The texture is an atlas built once with GDI: Segoe UI
//   glyphs, an anti-aliased disc (slider knobs, the toggle, rounded corners) and a solid white
//   block (plain rectangles). Every piece of GL state touched is saved and restored, and all GL
//   calls go straight to the driver, never through our own hooks.
//
//   Input: keys and the mouse are polled each frame. While the panel is open the game window is
//   subclassed so clicks and key presses stop reaching the game (releases still pass, so nothing
//   gets stuck), and the system cursor is replaced by a drawn one.

#include <windows.h>
#include <GL/gl.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "bike.h"
#include "camera.h"
#include "config.h"
#include "gamepad.h"
#include "hooks.h"
#include "log.h"
#include "mathlib.h"
#include "overlay.h"
#include "postfx.h"
#include "replay.h"
#include "sound_packs.h"
#include "texfilter.h"

namespace mxcam {
namespace {

// ===========================================================================
// OpenGL entry points (the driver's own) and constants past OpenGL 1.1
// ===========================================================================
constexpr GLenum kArrayBuffer             = 0x8892;
constexpr GLenum kArrayBufferBinding      = 0x8894;
constexpr GLenum kStreamDraw              = 0x88E0;
constexpr GLenum kVertexShader            = 0x8B31;
constexpr GLenum kFragmentShader          = 0x8B30;
constexpr GLenum kCompileStatus           = 0x8B81;
constexpr GLenum kLinkStatus              = 0x8B82;
constexpr GLenum kCurrentProgram          = 0x8B8D;
constexpr GLenum kTexture0                = 0x84C0;
constexpr GLenum kActiveTexture           = 0x84E0;
constexpr GLenum kVertexArrayBinding      = 0x85B5;
constexpr GLenum kDrawFramebuffer         = 0x8CA9;
constexpr GLenum kDrawFramebufferBinding  = 0x8CA6;
constexpr GLenum kSamplerBinding          = 0x8919;
constexpr GLenum kBlendDstRgb             = 0x80C8;
constexpr GLenum kBlendSrcRgb             = 0x80C9;
constexpr GLenum kBlendDstAlpha           = 0x80CA;
constexpr GLenum kBlendSrcAlpha           = 0x80CB;
constexpr GLenum kBlendEquationRgb        = 0x8009;
constexpr GLenum kBlendEquationAlpha      = 0x883D;
constexpr GLenum kFuncAdd                 = 0x8006;
constexpr GLenum kClampToEdge             = 0x812F;
constexpr GLenum kPixelUnpackBuffer       = 0x88EC;
constexpr GLenum kPixelUnpackBufferBinding = 0x88EF;
constexpr GLenum kFramebufferSrgb         = 0x8DB9;

struct GLApi {
    decltype(&::glGetIntegerv)        GetIntegerv;
    decltype(&::glGetBooleanv)        GetBooleanv;
    decltype(&::glIsEnabled)          IsEnabled;
    decltype(&::glEnable)             Enable;
    decltype(&::glDisable)            Disable;
    decltype(&::glViewport)           Viewport;
    decltype(&::glColorMask)          ColorMask;
    decltype(&::glGenTextures)        GenTextures;
    decltype(&::glDeleteTextures)     DeleteTextures;
    decltype(&::glBindTexture)        BindTexture;
    decltype(&::glTexParameteri)      TexParameteri;
    decltype(&::glTexImage2D)         TexImage2D;
    decltype(&::glPixelStorei)        PixelStorei;
    decltype(&::glDrawArrays)         DrawArrays;
    decltype(&::wglGetCurrentContext) GetCurrentContext;
    GLuint (APIENTRY* CreateShader)(GLenum);
    void   (APIENTRY* ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
    void   (APIENTRY* CompileShader)(GLuint);
    void   (APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*);
    void   (APIENTRY* DeleteShader)(GLuint);
    GLuint (APIENTRY* CreateProgram)();
    void   (APIENTRY* DeleteProgram)(GLuint);
    void   (APIENTRY* AttachShader)(GLuint, GLuint);
    void   (APIENTRY* BindAttribLocation)(GLuint, GLuint, const char*);
    void   (APIENTRY* LinkProgram)(GLuint);
    void   (APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*);
    void   (APIENTRY* UseProgram)(GLuint);
    GLint  (APIENTRY* GetUniformLocation)(GLuint, const char*);
    void   (APIENTRY* Uniform2f)(GLint, GLfloat, GLfloat);
    void   (APIENTRY* Uniform1i)(GLint, GLint);
    void   (APIENTRY* GenBuffers)(GLsizei, GLuint*);
    void   (APIENTRY* BindBuffer)(GLenum, GLuint);
    void   (APIENTRY* BufferData)(GLenum, ptrdiff_t, const void*, GLenum);
    void   (APIENTRY* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    void   (APIENTRY* EnableVertexAttribArray)(GLuint);
    void   (APIENTRY* ActiveTexture)(GLenum);
    void   (APIENTRY* BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
    void   (APIENTRY* BlendEquation)(GLenum);
    void   (APIENTRY* BlendEquationSeparate)(GLenum, GLenum);
    void   (APIENTRY* GenVertexArrays)(GLsizei, GLuint*);
    void   (APIENTRY* BindVertexArray)(GLuint);
    void   (APIENTRY* BindFramebuffer)(GLenum, GLuint);  // optional (GL 3.0)
    void   (APIENTRY* BindSampler)(GLuint, GLuint);      // optional (GL 3.3)
};

GLApi g_gl{};
bool  g_glLoaded    = false;
bool  g_glAvailable = false;

template <class T>
bool LoadFunction(T& slot, const char* name)
{
    slot = reinterpret_cast<T>(GetRealGLFunction(name));
    return slot != nullptr;
}

bool LoadGL()
{
    bool ok = true;
    ok = LoadFunction(g_gl.GetIntegerv, "glGetIntegerv") && ok;
    ok = LoadFunction(g_gl.GetBooleanv, "glGetBooleanv") && ok;
    ok = LoadFunction(g_gl.IsEnabled, "glIsEnabled") && ok;
    ok = LoadFunction(g_gl.Enable, "glEnable") && ok;
    ok = LoadFunction(g_gl.Disable, "glDisable") && ok;
    ok = LoadFunction(g_gl.Viewport, "glViewport") && ok;
    ok = LoadFunction(g_gl.ColorMask, "glColorMask") && ok;
    ok = LoadFunction(g_gl.GenTextures, "glGenTextures") && ok;
    ok = LoadFunction(g_gl.DeleteTextures, "glDeleteTextures") && ok;
    ok = LoadFunction(g_gl.BindTexture, "glBindTexture") && ok;
    ok = LoadFunction(g_gl.TexParameteri, "glTexParameteri") && ok;
    ok = LoadFunction(g_gl.TexImage2D, "glTexImage2D") && ok;
    ok = LoadFunction(g_gl.PixelStorei, "glPixelStorei") && ok;
    ok = LoadFunction(g_gl.DrawArrays, "glDrawArrays") && ok;
    ok = LoadFunction(g_gl.GetCurrentContext, "wglGetCurrentContext") && ok;
    ok = LoadFunction(g_gl.CreateShader, "glCreateShader") && ok;
    ok = LoadFunction(g_gl.ShaderSource, "glShaderSource") && ok;
    ok = LoadFunction(g_gl.CompileShader, "glCompileShader") && ok;
    ok = LoadFunction(g_gl.GetShaderiv, "glGetShaderiv") && ok;
    ok = LoadFunction(g_gl.DeleteShader, "glDeleteShader") && ok;
    ok = LoadFunction(g_gl.CreateProgram, "glCreateProgram") && ok;
    ok = LoadFunction(g_gl.DeleteProgram, "glDeleteProgram") && ok;
    ok = LoadFunction(g_gl.AttachShader, "glAttachShader") && ok;
    ok = LoadFunction(g_gl.BindAttribLocation, "glBindAttribLocation") && ok;
    ok = LoadFunction(g_gl.LinkProgram, "glLinkProgram") && ok;
    ok = LoadFunction(g_gl.GetProgramiv, "glGetProgramiv") && ok;
    ok = LoadFunction(g_gl.UseProgram, "glUseProgram") && ok;
    ok = LoadFunction(g_gl.GetUniformLocation, "glGetUniformLocation") && ok;
    ok = LoadFunction(g_gl.Uniform2f, "glUniform2f") && ok;
    ok = LoadFunction(g_gl.Uniform1i, "glUniform1i") && ok;
    ok = LoadFunction(g_gl.GenBuffers, "glGenBuffers") && ok;
    ok = LoadFunction(g_gl.BindBuffer, "glBindBuffer") && ok;
    ok = LoadFunction(g_gl.BufferData, "glBufferData") && ok;
    ok = LoadFunction(g_gl.VertexAttribPointer, "glVertexAttribPointer") && ok;
    ok = LoadFunction(g_gl.EnableVertexAttribArray, "glEnableVertexAttribArray") && ok;
    ok = LoadFunction(g_gl.ActiveTexture, "glActiveTexture") && ok;
    ok = LoadFunction(g_gl.BlendFuncSeparate, "glBlendFuncSeparate") && ok;
    ok = LoadFunction(g_gl.BlendEquation, "glBlendEquation") && ok;
    ok = LoadFunction(g_gl.BlendEquationSeparate, "glBlendEquationSeparate") && ok;
    ok = LoadFunction(g_gl.GenVertexArrays, "glGenVertexArrays") && ok;
    ok = LoadFunction(g_gl.BindVertexArray, "glBindVertexArray") && ok;
    LoadFunction(g_gl.BindFramebuffer, "glBindFramebuffer");
    LoadFunction(g_gl.BindSampler, "glBindSampler");
    return ok;
}

// ===========================================================================
// Saved GL state
// ===========================================================================
struct SavedState {
    GLint     program = 0, vao = 0, arrayBuffer = 0, activeTexture = 0, texture = 0, sampler = 0, drawFramebuffer = 0;
    GLint     viewport[4] = {};
    GLint     blendSrcRgb = 0, blendDstRgb = 0, blendSrcAlpha = 0, blendDstAlpha = 0, blendEqRgb = 0, blendEqAlpha = 0;
    GLboolean colorMask[4] = {};
    bool      blend = false, depthTest = false, cullFace = false, scissorTest = false, stencilTest = false, srgb = false;
};

void SaveState(SavedState& s)
{
    g_gl.GetIntegerv(kCurrentProgram, &s.program);
    g_gl.GetIntegerv(kVertexArrayBinding, &s.vao);
    g_gl.GetIntegerv(kArrayBufferBinding, &s.arrayBuffer);
    g_gl.GetIntegerv(kActiveTexture, &s.activeTexture);
    g_gl.ActiveTexture(kTexture0);
    g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s.texture);
    if (g_gl.BindSampler)
        g_gl.GetIntegerv(kSamplerBinding, &s.sampler);
    if (g_gl.BindFramebuffer)
        g_gl.GetIntegerv(kDrawFramebufferBinding, &s.drawFramebuffer);
    g_gl.GetIntegerv(GL_VIEWPORT, s.viewport);
    g_gl.GetIntegerv(kBlendSrcRgb, &s.blendSrcRgb);
    g_gl.GetIntegerv(kBlendDstRgb, &s.blendDstRgb);
    g_gl.GetIntegerv(kBlendSrcAlpha, &s.blendSrcAlpha);
    g_gl.GetIntegerv(kBlendDstAlpha, &s.blendDstAlpha);
    g_gl.GetIntegerv(kBlendEquationRgb, &s.blendEqRgb);
    g_gl.GetIntegerv(kBlendEquationAlpha, &s.blendEqAlpha);
    g_gl.GetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
    s.blend       = g_gl.IsEnabled(GL_BLEND) != GL_FALSE;
    s.depthTest   = g_gl.IsEnabled(GL_DEPTH_TEST) != GL_FALSE;
    s.cullFace    = g_gl.IsEnabled(GL_CULL_FACE) != GL_FALSE;
    s.scissorTest = g_gl.IsEnabled(GL_SCISSOR_TEST) != GL_FALSE;
    s.stencilTest = g_gl.IsEnabled(GL_STENCIL_TEST) != GL_FALSE;
    s.srgb        = g_gl.BindFramebuffer && g_gl.IsEnabled(kFramebufferSrgb) != GL_FALSE;  // GL 3.0+
}

void SetCapability(GLenum capability, bool enabled)
{
    if (enabled)
        g_gl.Enable(capability);
    else
        g_gl.Disable(capability);
}

void RestoreState(const SavedState& s)
{
    g_gl.BindVertexArray(s.vao);
    g_gl.BindBuffer(kArrayBuffer, s.arrayBuffer);
    g_gl.UseProgram(s.program);
    g_gl.BindTexture(GL_TEXTURE_2D, s.texture);  // texture unit 0 is still active here
    if (g_gl.BindSampler)
        g_gl.BindSampler(0, s.sampler);
    g_gl.ActiveTexture(s.activeTexture);
    if (g_gl.BindFramebuffer)
        g_gl.BindFramebuffer(kDrawFramebuffer, s.drawFramebuffer);
    g_gl.Viewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    g_gl.BlendFuncSeparate(s.blendSrcRgb, s.blendDstRgb, s.blendSrcAlpha, s.blendDstAlpha);
    g_gl.BlendEquationSeparate(s.blendEqRgb, s.blendEqAlpha);
    g_gl.ColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    SetCapability(GL_BLEND, s.blend);
    SetCapability(GL_DEPTH_TEST, s.depthTest);
    SetCapability(GL_CULL_FACE, s.cullFace);
    SetCapability(GL_SCISSOR_TEST, s.scissorTest);
    SetCapability(GL_STENCIL_TEST, s.stencilTest);
    if (g_gl.BindFramebuffer)
        SetCapability(kFramebufferSrgb, s.srgb);
}

// ===========================================================================
// Atlas: solid block + anti-aliased disc + glyphs
// ===========================================================================
constexpr int kAtlasSize = 2048;
constexpr int kDiscX = 16, kDiscY = 4, kDiscSize = 64;
constexpr int kGlyphTop = kDiscY + kDiscSize + 8;

enum FontId { kFontTitle, kFontBody, kFontValue, kFontSmall, kFontCount };

struct Glyph {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    float width = 0, height = 0, bearing = 0, advance = 0;
    bool  valid = false;
};

struct Font {
    Glyph glyphs[256];
    float lineHeight = 0;
};

Font  g_fonts[kFontCount];
float g_atlasScale = 0;

bool BuildAtlas(GLuint texture, float scale)
{
    std::vector<uint8_t> coverage(kAtlasSize * kAtlasSize, 0);

    // Solid block for plain rectangles (sampled at its centre, away from the edges).
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            coverage[y * kAtlasSize + x] = 255;

    // Anti-aliased disc: knobs, the toggle and rounded corners are cut from it.
    const float radius = kDiscSize * 0.5f - 1.0f;
    for (int y = 0; y < kDiscSize; ++y) {
        for (int x = 0; x < kDiscSize; ++x) {
            const float dx = x + 0.5f - kDiscSize * 0.5f, dy = y + 0.5f - kDiscSize * 0.5f;
            const float alpha = Clamp(radius - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            coverage[(kDiscY + y) * kAtlasSize + kDiscX + x] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
        }
    }

    // Glyphs, rendered white-on-black by GDI with grayscale anti-aliasing.
    BITMAPINFO info = {};
    info.bmiHeader.biSize        = sizeof info.bmiHeader;
    info.bmiHeader.biWidth       = kAtlasSize;
    info.bmiHeader.biHeight      = -kAtlasSize;  // top-down
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return false;
    }
    HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    memset(bits, 0, kAtlasSize * kAtlasSize * 4);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);

    struct FontSpec { int pixels; int weight; };
    const FontSpec specs[kFontCount] = {
        { 26, FW_BOLD },      // kFontTitle
        { 16, FW_NORMAL },    // kFontBody
        { 16, FW_SEMIBOLD },  // kFontValue
        { 13, FW_SEMIBOLD },  // kFontSmall (smaller gets crunchy once GDI hinting kicks in)
    };
    const int pad = 2;
    int penX = 0, penY = kGlyphTop, rowHeight = 0;
    bool fits = true;

    for (int f = 0; f < kFontCount && fits; ++f) {
        HFONT font = CreateFontW(-static_cast<int>(std::lround(specs[f].pixels * scale)), 0, 0, 0, specs[f].weight,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ previousFont = SelectObject(dc, font);
        TEXTMETRICW metrics = {};
        GetTextMetricsW(dc, &metrics);
        Font& out = g_fonts[f];
        out = Font{};
        out.lineHeight = static_cast<float>(metrics.tmHeight);

        // Printable ASCII plus the degree sign and middle dot.
        for (int code = 32; code <= 0xB7 && fits; ++code) {
            if (code > 126 && code != 0xB0 && code != 0xB7)
                continue;
            const wchar_t ch = static_cast<wchar_t>(code);
            ABC abc = {};
            if (!GetCharABCWidthsW(dc, ch, ch, &abc)) {
                SIZE size = {};
                GetTextExtentPoint32W(dc, &ch, 1, &size);
                abc.abcA = 0;
                abc.abcB = static_cast<UINT>(size.cx);
                abc.abcC = 0;
            }
            const int cellWidth = static_cast<int>(abc.abcB > 0 ? abc.abcB : 1) + 2 * pad;
            const int cellHeight = metrics.tmHeight;
            if (penX + cellWidth > kAtlasSize) {
                penX = 0;
                penY += rowHeight + pad;
                rowHeight = 0;
            }
            if (penY + cellHeight > kAtlasSize) {
                fits = false;
                break;
            }
            TextOutW(dc, penX + pad - abc.abcA, penY, &ch, 1);

            Glyph& g  = out.glyphs[code];
            g.u0      = static_cast<float>(penX) / kAtlasSize;
            g.v0      = static_cast<float>(penY) / kAtlasSize;
            g.u1      = static_cast<float>(penX + cellWidth) / kAtlasSize;
            g.v1      = static_cast<float>(penY + cellHeight) / kAtlasSize;
            g.width   = static_cast<float>(cellWidth);
            g.height  = static_cast<float>(cellHeight);
            g.bearing = static_cast<float>(abc.abcA - pad);
            g.advance = static_cast<float>(abc.abcA + static_cast<int>(abc.abcB) + abc.abcC);
            g.valid   = true;

            penX += cellWidth + pad;
            rowHeight = rowHeight > cellHeight ? rowHeight : cellHeight;
        }
        penX = 0;
        penY += rowHeight + pad;
        rowHeight = 0;
        SelectObject(dc, previousFont);
        DeleteObject(font);
    }
    GdiFlush();

    const uint8_t* pixels = static_cast<const uint8_t*>(bits);
    for (int i = kGlyphTop * kAtlasSize; i < kAtlasSize * kAtlasSize; ++i)
        coverage[i] = pixels[i * 4 + 2];  // grayscale anti-aliasing: any channel will do (BGRA)
    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    if (!fits)
        return false;

    // RGBA, white everywhere: the shader multiplies the vertex colour by the alpha.
    std::vector<uint8_t> rgba(kAtlasSize * kAtlasSize * 4, 255);
    for (int i = 0; i < kAtlasSize * kAtlasSize; ++i)
        rgba[i * 4 + 3] = coverage[i];

    GLint alignment = 4, rowLength = 0, unpackBuffer = 0;
    g_gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    g_gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    g_gl.GetIntegerv(kPixelUnpackBufferBinding, &unpackBuffer);
    g_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    g_gl.BindBuffer(kPixelUnpackBuffer, 0);

    g_gl.BindTexture(GL_TEXTURE_2D, texture);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
    g_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kAtlasSize, kAtlasSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    g_gl.BindBuffer(kPixelUnpackBuffer, unpackBuffer);
    g_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    g_gl.PixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    g_atlasScale = scale;
    return true;
}

// ===========================================================================
// GPU resources (per GL context)
// ===========================================================================
const char* const kVertexBody =
    "IN vec2 a_pos;\n"
    "IN vec2 a_uv;\n"
    "IN vec4 a_color;\n"
    "OUT vec2 v_uv;\n"
    "OUT vec4 v_color;\n"
    "uniform vec2 u_screen;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_color = a_color;\n"
    "    gl_Position = vec4(a_pos.x / u_screen.x * 2.0 - 1.0, 1.0 - a_pos.y / u_screen.y * 2.0, 0.0, 1.0);\n"
    "}\n";

const char* const kFragmentBody =
    "IN vec2 v_uv;\n"
    "IN vec4 v_color;\n"
    "uniform sampler2D u_atlas;\n"
    "void main() { FRAG_COLOR = vec4(v_color.rgb, v_color.a * TEX(u_atlas, v_uv).a); }\n";

// The same shader in the three GLSL dialects a game context might accept (core 3.2+, 3.0/3.1,
// and 2.x compatibility); the first one that compiles wins.
struct ShaderDialect {
    const char* vertexHeader;
    const char* fragmentHeader;
};
const ShaderDialect kDialects[] = {
    { "#version 150\n#define IN in\n#define OUT out\n",
      "#version 150\n#define IN in\n#define TEX texture\nout vec4 o_color;\n#define FRAG_COLOR o_color\n" },
    { "#version 130\n#define IN in\n#define OUT out\n",
      "#version 130\n#define IN in\n#define TEX texture\nout vec4 o_color;\n#define FRAG_COLOR o_color\n" },
    { "#version 120\n#define IN attribute\n#define OUT varying\n",
      "#version 120\n#define IN varying\n#define TEX texture2D\n#define FRAG_COLOR gl_FragColor\n" },
};

struct Vertex {
    float    x, y, u, v;
    uint32_t color;  // RGBA bytes
};

struct GpuResources {
    HGLRC  context = nullptr;
    bool   failed  = false;
    GLuint program = 0, vao = 0, vbo = 0, texture = 0;
    GLint  screenUniform = -1, atlasUniform = -1;
};
GpuResources g_gpu;

bool Compiled(GLuint shader)
{
    GLint status = 0;
    g_gl.GetShaderiv(shader, kCompileStatus, &status);
    return status != 0;
}

GLuint CompileProgram()
{
    for (const ShaderDialect& dialect : kDialects) {
        const char* vertexSource[] = { dialect.vertexHeader, kVertexBody };
        const char* fragmentSource[] = { dialect.fragmentHeader, kFragmentBody };
        const GLuint vs = g_gl.CreateShader(kVertexShader);
        const GLuint fs = g_gl.CreateShader(kFragmentShader);
        g_gl.ShaderSource(vs, 2, vertexSource, nullptr);
        g_gl.ShaderSource(fs, 2, fragmentSource, nullptr);
        g_gl.CompileShader(vs);
        g_gl.CompileShader(fs);

        GLuint program = 0;
        if (Compiled(vs) && Compiled(fs)) {
            program = g_gl.CreateProgram();
            g_gl.AttachShader(program, vs);
            g_gl.AttachShader(program, fs);
            g_gl.BindAttribLocation(program, 0, "a_pos");
            g_gl.BindAttribLocation(program, 1, "a_uv");
            g_gl.BindAttribLocation(program, 2, "a_color");
            g_gl.LinkProgram(program);
            GLint linked = 0;
            g_gl.GetProgramiv(program, kLinkStatus, &linked);
            if (!linked) {
                g_gl.DeleteProgram(program);
                program = 0;
            }
        }
        g_gl.DeleteShader(vs);  // freed together with the program
        g_gl.DeleteShader(fs);
        if (program) {
            Log("overlay: ready (GLSL %.11s)", dialect.vertexHeader + 1);
            return program;
        }
    }
    return 0;
}

// Creates (or recreates, after a context change) everything the panel draws with.
bool EnsureResources(float scale)
{
    const HGLRC context = g_gl.GetCurrentContext();
    if (context != g_gpu.context) {
        // New context: the old objects belonged to the old one.
        g_gpu = GpuResources{};
        g_gpu.context = context;
        g_atlasScale = 0;
    }
    if (g_gpu.failed)
        return false;

    if (!g_gpu.program) {
        g_gpu.program = CompileProgram();
        if (!g_gpu.program) {
            Log("overlay: could not compile the panel shader; settings panel disabled");
            g_gpu.failed = true;
            return false;
        }
        g_gpu.screenUniform = g_gl.GetUniformLocation(g_gpu.program, "u_screen");
        g_gpu.atlasUniform  = g_gl.GetUniformLocation(g_gpu.program, "u_atlas");
    }
    if (!g_gpu.vao) {
        g_gl.GenVertexArrays(1, &g_gpu.vao);
        g_gl.GenBuffers(1, &g_gpu.vbo);
        g_gl.BindVertexArray(g_gpu.vao);
        g_gl.BindBuffer(kArrayBuffer, g_gpu.vbo);
        g_gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, x)));
        g_gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, u)));
        g_gl.VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, color)));
        g_gl.EnableVertexAttribArray(0);
        g_gl.EnableVertexAttribArray(1);
        g_gl.EnableVertexAttribArray(2);
    }
    if (!g_gpu.texture)
        g_gl.GenTextures(1, &g_gpu.texture);
    if (std::fabs(scale - g_atlasScale) > 0.01f && !BuildAtlas(g_gpu.texture, scale)) {
        Log("overlay: could not build the font atlas; settings panel disabled");
        g_gpu.failed = true;
        return false;
    }
    return true;
}

// ===========================================================================
// Drawing primitives (pixel coordinates, top-left origin)
// ===========================================================================
std::vector<Vertex> g_batch;

uint32_t Rgba(int r, int g, int b, float a)
{
    const uint32_t alpha = static_cast<uint32_t>(Clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    return static_cast<uint32_t>(r) | static_cast<uint32_t>(g) << 8 | static_cast<uint32_t>(b) << 16 | alpha << 24;
}
uint32_t White(float a) { return Rgba(255, 255, 255, a); }
uint32_t Gray(int level, float a = 1.0f) { return Rgba(level, level, level, a); }
// Colour theme: dark navy-charcoal panels with a blue glow toward the bottom, a royal blue border,
// royal blue for whatever is on or selected, white text.
const uint32_t kPanelColor = Rgba(13, 17, 26, 0.96f);
const uint32_t kPanelSolid = Rgba(13, 17, 26, 1.0f);
const uint32_t kBlue       = Rgba(26, 100, 214, 1.0f);   // on, selected, primary
const uint32_t kBlueHot    = Rgba(54, 130, 238, 1.0f);
const uint32_t kBorder     = Rgba(30, 94, 206, 0.95f);   // panel outline
const uint32_t kLine       = Rgba(36, 78, 148, 0.6f);    // separators, idle outlines
const uint32_t kTrack      = Rgba(32, 86, 176, 1.0f);    // slider track
const uint32_t kLabel      = Rgba(222, 228, 238, 1.0f);
const uint32_t kDim        = Rgba(112, 124, 146, 1.0f);
const uint32_t kSection    = Rgba(140, 164, 204, 1.0f);

void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t color)
{
    const Vertex a{ x0, y0, u0, v0, color }, b{ x1, y0, u1, v0, color };
    const Vertex c{ x1, y1, u1, v1, color }, d{ x0, y1, u0, v1, color };
    g_batch.insert(g_batch.end(), { a, b, c, a, c, d });
}

void Rect(float x, float y, float w, float h, uint32_t color)
{
    if (w <= 0 || h <= 0)
        return;
    const float u = 4.0f / kAtlasSize;  // middle of the solid block
    Quad(std::round(x), std::round(y), std::round(x + w), std::round(y + h), u, u, u, u, color);
}

constexpr float kDiscU0 = static_cast<float>(kDiscX) / kAtlasSize;
constexpr float kDiscV0 = static_cast<float>(kDiscY) / kAtlasSize;
constexpr float kDiscU1 = static_cast<float>(kDiscX + kDiscSize) / kAtlasSize;
constexpr float kDiscV1 = static_cast<float>(kDiscY + kDiscSize) / kAtlasSize;
constexpr float kDiscUM = (kDiscU0 + kDiscU1) * 0.5f;
constexpr float kDiscVM = (kDiscV0 + kDiscV1) * 0.5f;

void Disc(float cx, float cy, float r, uint32_t color)
{
    Quad(cx - r, cy - r, cx + r, cy + r, kDiscU0, kDiscV0, kDiscU1, kDiscV1, color);
}

// Rectangle with anti-aliased rounded corners cut from the disc.
void RoundedRect(float x, float y, float w, float h, float r, uint32_t color)
{
    r = std::floor(std::fmin(r, std::fmin(w, h) * 0.5f));
    Quad(x, y, x + r, y + r, kDiscU0, kDiscV0, kDiscUM, kDiscVM, color);
    Quad(x + w - r, y, x + w, y + r, kDiscUM, kDiscV0, kDiscU1, kDiscVM, color);
    Quad(x, y + h - r, x + r, y + h, kDiscU0, kDiscVM, kDiscUM, kDiscV1, color);
    Quad(x + w - r, y + h - r, x + w, y + h, kDiscUM, kDiscVM, kDiscU1, kDiscV1, color);
    Rect(x + r, y, w - 2 * r, h, color);
    Rect(x, y + r, r, h - 2 * r, color);
    Rect(x + w - r, y + r, r, h - 2 * r, color);
}

float LineHeight(FontId font) { return g_fonts[font].lineHeight; }

float TextWidth(FontId font, const char* text, float tracking)
{
    float width = 0;
    int count = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p, ++count)
        width += g_fonts[font].glyphs[*p].advance;
    return count ? width + tracking * (count - 1) : 0;
}

constexpr int kMaxWrapLines = 6;
constexpr int kWrapLineSize = 160;

// Splits text into lines no wider than width, breaking at spaces. Returns the line count.
int WrapText(FontId font, const char* text, float width, char (&lines)[kMaxWrapLines][kWrapLineSize])
{
    int count = 0;
    while (*text && count < kMaxWrapLines) {
        char* line = lines[count++];
        size_t length = 0;
        while (*text) {
            const char* end = text;
            while (*end && *end != ' ')
                ++end;
            const size_t word = static_cast<size_t>(end - text);
            const size_t joined = length + (length ? 1 : 0) + word;
            if (joined >= kWrapLineSize)
                break;
            char candidate[kWrapLineSize];
            snprintf(candidate, sizeof candidate, "%.*s%s%.*s", static_cast<int>(length), line, length ? " " : "",
                     static_cast<int>(word), text);
            if (length && TextWidth(font, candidate, 0) > width)
                break;  // the word starts the next line
            memcpy(line, candidate, joined + 1);
            length = joined;
            text = *end ? end + 1 : end;
        }
        line[length] = '\0';
    }
    return count;
}

void Text(FontId font, float x, float y, const char* text, uint32_t color, float tracking)
{
    float pen = x;
    y = std::round(y);
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        const Glyph& g = g_fonts[font].glyphs[*p];
        if (g.valid && *p != ' ') {
            const float gx = std::round(pen + g.bearing);
            Quad(gx, y, gx + g.width, y + g.height, g.u0, g.v0, g.u1, g.v1, color);
        }
        pen += g.advance + tracking;
    }
}

// ===========================================================================
// Panel state, input and window subclass
// ===========================================================================
enum class RowKind { Slider, Toggle, Choice, Key, Pad };

struct RowDef {
    int              tab;
    const char*      section;   // starts a new section when set
    const char*      label;
    RowKind          kind;
    double Config::* number;    // Slider
    bool Config::*   flag;      // Toggle / Choice of two
    double           min, max, step;
    double           display;   // multiplier for the shown value (e.g. fraction -> %)
    int              decimals;
    const char*      unit;
    const char*      zeroText;  // shown instead of the minimum, e.g. "Off"
    const char*      offText;   // Choice: the first two options
    const char*      onText;
    bool             customOnly;  // only does something with the custom camera
    int Config::*    key = nullptr;  // Key: a virtual-key code, picked by pressing the key
    const char*      help = nullptr; // shown beside the panel while the row is selected / under the mouse
    bool             locked = false; // work in progress: drawn dimmed and can't be changed
    int Config::*    option = nullptr;     // Choice of three or four: 0 / 1 / 2 (/ 3), instead of flag
    const char*      thirdText = nullptr;  // its third option
    const char*      fourthText = nullptr; // and fourth, if it has one
    double Config::* sideNumber = nullptr; // Slider: what it sets instead with the helmet camera on a side mount
    double Config::* chestNumber = nullptr; // and on the chest mount
    bool             clock = false;          // Slider: hours, shown as a time of day (HH:MM)
    int              column = -1;            // starts this column of its tab (later rows follow it)
    int Config::*    pad = nullptr;          // Pad: a gamepad::Control, picked by pressing it

    RowDef Col(int index) const
    {
        RowDef def = *this;
        def.column = index;
        return def;
    }
    RowDef Side(double Config::* field) const
    {
        RowDef def = *this;
        def.sideNumber = field;
        return def;
    }
    RowDef Chest(double Config::* field) const
    {
        RowDef def = *this;
        def.chestNumber = field;
        return def;
    }

    RowDef Help(const char* text) const
    {
        RowDef def = *this;
        def.help = text;
        return def;
    }
    RowDef Locked() const
    {
        RowDef def = *this;
        def.locked = true;
        return def;
    }
    RowDef Clock() const
    {
        RowDef def = *this;
        def.clock = true;
        return def;
    }
};

RowDef Slider(int tab, const char* section, const char* label, double Config::* field, double min, double max,
              double step, double display, int decimals, const char* unit, const char* zeroText = nullptr,
              bool customOnly = false)
{
    return { tab, section, label, RowKind::Slider, field, nullptr, min, max, step, display, decimals, unit,
             zeroText, nullptr, nullptr, customOnly };
}

RowDef Toggle(int tab, const char* section, const char* label, bool Config::* flag, bool customOnly = false)
{
    return { tab, section, label, RowKind::Toggle, nullptr, flag, 0, 1, 1, 1, 0, "", nullptr, nullptr, nullptr, customOnly };
}

RowDef Choice(int tab, const char* section, const char* label, bool Config::* flag, const char* offText, const char* onText)
{
    return { tab, section, label, RowKind::Choice, nullptr, flag, 0, 1, 1, 1, 0, "", nullptr, offText, onText, false };
}

RowDef Choice3(int tab, const char* section, const char* label, int Config::* option, const char* first,
               const char* second, const char* third)
{
    RowDef def = Choice(tab, section, label, nullptr, first, second);
    def.option = option;
    def.thirdText = third;
    return def;
}

RowDef Choice4(int tab, const char* section, const char* label, int Config::* option, const char* first,
               const char* second, const char* third, const char* fourth)
{
    RowDef def = Choice3(tab, section, label, option, first, second, third);
    def.fourthText = fourth;
    return def;
}

// The field a slider sets: per helmet camera mount where it has them.
double Config::* SliderField(const RowDef& def, const Config& cfg)
{
    if (def.chestNumber && cfg.helmetMount == 3)
        return def.chestNumber;
    if (def.sideNumber && (cfg.helmetMount == 1 || cfg.helmetMount == 2))
        return def.sideNumber;
    return def.number;
}

RowDef KeyRow(int tab, const char* section, const char* label, int Config::* key)
{
    return { tab, section, label, RowKind::Key, nullptr, nullptr, 0, 1, 1, 1, 0, "", nullptr, nullptr, nullptr, false, key };
}

RowDef PadRow(int tab, const char* section, const char* label, int Config::* control)
{
    RowDef def = KeyRow(tab, section, label, nullptr);
    def.kind = RowKind::Pad;
    def.pad = control;
    return def;
}

const char* const kTabs[] = { "FIRST PERSON", "GRAPHICS", "REPLAY", "KEYBINDS", "OTHER" };
constexpr int kTabFirstPerson = 0, kTabGraphics = 1, kTabReplay = 2, kTabKeybinds = 3, kTabOther = 4;
constexpr int kTabCount = static_cast<int>(sizeof kTabs / sizeof kTabs[0]);

// Each tab's rows, column by column (.Col starts the next one).
const RowDef kRows[] = {
    // ======== FIRST PERSON ========
    Toggle(0, "CAMERA",          "First person mod",     &Config::enabled)
        .Help("The first-person camera mod (its key is on the KEYBINDS tab). Off gives you the game's own "
              "cameras and replays untouched, and only the shaders stay on.")
        .Col(0),
    Choice(0, nullptr,           "Camera",               &Config::customCamera, "GAME", "CUSTOM")
        .Help("GAME adjusts the game's own first-person camera (and its 1st Person Angle). CUSTOM builds "
              "the view from your bike instead. The first time, ride a few metres so it can find your bike; "
              "after that it knows it as soon as you spawn."),
    Slider(0, nullptr,           "Look-down angle",      &Config::cameraAngleDeg,   -10.0, 30.0, 0.5,  1.0,   1, "\xB0", nullptr, true)
        .Help("Custom camera: how far the view tilts down. Negative tilts it up. (The GAME camera uses "
              "the game's own 1st Person Angle.)"),
    Slider(0, "STABILIZATION",   "Horizon lock",         &Config::horizonLock,      0.0,   1.0,  0.01, 100.0, 0, "%", "Off")
        .Help("How much of the bike's lean is removed from the view. 0% leans with the bike, 100% keeps "
              "the horizon level."),
    Slider(0, nullptr,           "Pitch lock",           &Config::pitchLock,        0.0,   1.0,  0.01, 100.0, 0, "%", "Off")
        .Help("How much of the bike's nose up / down tilt (wheelies, jump faces) is removed. 0% tilts "
              "with the bike, 100% keeps the view steady."),
    Slider(0, nullptr,           "Steadiness",           &Config::steadiness,       0.0,   2.0,  0.01, 100.0, 0, "%", "Off")
        .Help("Keeps the view calm: the quick jolts of the bike (bumps, landings, the suspension diving "
              "under braking) don't reach it. It still eases into a corner's lean or a wheelie, as far as "
              "the locks above let it. Off follows every jolt."),
    Slider(0, "HEAD POSITION",   "Head forward / back",  &Config::headForward,      -3.0,  3.0,  0.01, 1.0,   2, " m")
        .Help("Moves your eyes forward (+) or back (-) along the bike."),
    Slider(0, nullptr,           "Head height",          &Config::headHeight,       -3.0,  3.0,  0.01, 1.0,   2, " m")
        .Help("Moves your eyes up (+) or down (-)."),
    Toggle(0, nullptr,           "Hide torso & head",    &Config::hideOwnBody)
        .Help("Hides your own rider's torso and head (helmet included), so a high FOV doesn't show "
              "them from the inside. Arms and legs stay, and your shadow keeps them all. Only yours: "
              "other riders keep theirs."),
    Slider(0, "CORNERS",         "Look to apex",         &Config::apexLook,         0.0,   1.0,  0.01, 100.0, 0, "%", "Off")
        .Help("In corners the head turns toward the apex, the way the bike is turning: more the "
              "tighter the corner. Higher looks further into the turn (100% looks about a second "
              "down your line, at most 45\xB0). Off at walking pace and in the air.")
        .Col(1),
    Slider(0, nullptr,           "Apex turn speed",      &Config::apexSpeed,        0.5,   5.0,  0.5, 1.0,   1, "")
        .Help("How quickly the head turns toward the apex and back to straight ahead after the "
              "corner. It eases in and out gently either way; lower is more gradual: 2.5 takes "
              "about a second to settle, 1 about three. Too low and it lags behind short corners."),
    Slider(0, nullptr,           "Anticipation",         &Config::apexAnticipation, 0.0,   0.5,  0.05, 1.0,  2, " s",
           "Work in progress")
        .Help("Work in progress, off for now. It will set how early the head turns: reading where the "
              "corner is going, so it turns in sooner as a corner tightens and comes back to straight "
              "ahead sooner as it opens up on the exit.")
        .Locked(),
    Toggle(0, "LOOKING AROUND",  "Controller look",      &Config::controllerLook)
        .Help("Look around with a controller: the d-pad's left / right to look sideways and down to "
              "look behind, unless you bind other buttons, paddles or stick directions on the KEYBINDS "
              "tab. Release to look ahead again."),
    Slider(0, nullptr,           "Side look angle",      &Config::lookSideDeg,      30.0,   90.0, 1.0, 1.0,   0, "\xB0")
        .Help("How far the head turns when looking left / right with the controller."),
    Slider(0, nullptr,           "Turn speed",           &Config::lookTurnSpeed,    2.0,   40.0, 1.0, 1.0,   0, "")
        .Help("How quickly the head turns to look left / right / back and ahead again. Higher is "
              "faster: 12 turns 90\xB0 in about a quarter of a second, 40 is nearly instant."),
    Slider(0, nullptr,           "Sideways: forward",    &Config::lookSideForward,  -1.0,  1.0,  0.01, 1.0,   2, " m", nullptr, true)
        .Help("Custom camera: moves your eyes forward (+) or back (-) while looking left / right."),
    Slider(0, nullptr,           "Sideways: height",     &Config::lookSideHeight,   -1.0,  1.0,  0.01, 1.0,   2, " m", nullptr, true)
        .Help("Custom camera: moves your eyes up (+) or down (-) while looking left / right."),
    Slider(0, nullptr,           "Back: forward",        &Config::lookBackForward,  -1.0,  1.0,  0.01, 1.0,   2, " m", nullptr, true)
        .Help("Custom camera: moves your eyes forward (+) or back (-) while looking back."),
    Slider(0, nullptr,           "Back: height",         &Config::lookBackHeight,   -1.0,  1.0,  0.01, 1.0,   2, " m", nullptr, true)
        .Help("Custom camera: moves your eyes up (+) or down (-) while looking back."),
    Slider(0, "IN THE AIR",      "Looseness",            &Config::airLooseness,     0.0,   1.0,  0.01, 100.0, 0, "%", "Off")
        .Help("In the air only: the view keeps pointing forward while the bike swings left / right "
              "(scrubs, whips), by up to 15\xB0 x this. Off follows the bike tightly.")
        .Col(2),
    Slider(0, nullptr,           "Whip lock",            &Config::whipLock,         0.0,   1.0,  0.01, 100.0, 0, "%", "Off")
        .Help("In the air only: holds the view on your direction of travel, horizon level, at your "
              "normal look-down angle, while the bike swings, leans and tilts under it. 100% keeps "
              "it completely straight; it eases back onto the bike after landing."),
    Slider(0, "SMOOTHING",       "Rotation response",    &Config::rotationResponse, 0.0,   40.0,  0.5,    1.0,    1, " /s", "Off")
        .Help("How fast the view's rotation catches up to the bike. Higher is sharper, lower is "
              "floatier. Works on the ground and in the air."),
    Slider(0, nullptr,           "Max lag",              &Config::maxLagDeg,        0.0,   90.0,  0.5,    1.0,    1, "\xB0")
        .Help("The furthest the smoothed view can fall behind the bike. Past this it turns with the "
              "bike instead of trailing further."),
    Slider(0, "NECK & IMPACTS",  "Landing compression",  &Config::compressionPerG,  0.0,   0.05,  0.0005, 1000.0, 1, " mm/g", "Off")
        .Help("How much your head sinks on landings and big hits (and lifts in the air)."),
    Slider(0, nullptr,           "Max compression",      &Config::maxCompression,   0.0,   0.2,   0.001,  1000.0, 0, " mm")
        .Help("The most your head can sink on a landing."),
    Slider(0, nullptr,           "Neck stiffness",       &Config::springHz,         0.5,   10.0,  0.1,    1.0,    1, " Hz")
        .Help("How quickly your head springs back after a hit. Higher is a tighter neck."),
    Slider(0, nullptr,           "Neck damping",         &Config::dampingRatio,     0.05,  2.0,   0.05,   1.0,    2, "")
        .Help("How much your head bounces after a hit. Lower is bouncier, 1.00 settles with no bounce."),
    Slider(0, nullptr,           "Head nod",             &Config::nodDegPerMeter,   0.0,   150.0, 1.0,    1.0,    0, "\xB0" "/m", "Off")
        .Help("How far your head nods forward as your neck compresses on a landing."),
    Slider(0, nullptr,           "Height smoothing",     &Config::verticalResponse, 0.0,   60.0,  1.0,    1.0,    0, " /s", "Off")
        .Help("Smooths small up / down head jitter. Higher follows the bike's height more closely."),
    // ======== GRAPHICS ========
    Toggle(1, "SHADERS",         "Shaders",              &Config::shaders)
        .Help("Turns every effect on this tab on or off (its key is on the KEYBINDS tab). Off is the game "
              "exactly as it looks without the mod.")
        .Col(0),
    Slider(1, "TIME OF DAY",     "Time of day",          &Config::timeOfDay,        -1.0,  24.0,  0.25,   1.0,    0, "", "Game")
        .Clock()
        .Help("Sets the hour: the sun moves across the sky, sunsets turn the light orange, and at "
              "night the sky fills with stars and a moon lights the track. Game keeps the game's "
              "own lighting."),
    Toggle(1, nullptr,           "Move the sun",         &Config::moveSun)
        .Help("The time of day also moves the game's sun, so lighting and shading on bikes and "
              "objects follow it. Off only changes the colours and the sky."),
    Slider(1, nullptr,           "Night brightness",     &Config::nightBrightness,  0.0,   2.0,   0.05,   100.0,  0, "%")
        .Help("How much the moon lights the track at night. Lower is darker (the headlight stands "
              "out more), 0% is pitch black."),
    Slider(1, "HEADLIGHT",       "Headlight",            &Config::headlight,        0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("A headlight on your bike's front plate that lights the track ahead when the time of "
              "day makes it dark. It turns with the bike, not your head."),
    Slider(1, nullptr,           "Beam width",           &Config::headlightBeamDeg, 10.0,  90.0,  1.0,    2.0,    0, "\xB0")
        .Help("How far the headlight's beam spreads to the sides. It always lights the ground from "
              "just in front of the plate out, narrower only makes it skinnier."),
    Slider(1, nullptr,           "Beam aim",             &Config::headlightAimDeg,  -20.0, 15.0,  0.5,    1.0,    1, "\xB0")
        .Help("Where the headlight is aimed. Up shows more of what's ahead (the light reaches further "
              "and lights up jump faces), down keeps it close in front of the bike. 0 is level."),
    Slider(1, "GLOW",            "Sun rays",             &Config::sunRays,          0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Light shafts from the sun when it's on screen."),
    Slider(1, nullptr,           "Bloom",                &Config::bloom,            0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("A soft glow around bright parts of the picture: the sky, the sun, white plastics."),
    Toggle(1, nullptr,           "Hide game flare",      &Config::hideGameFlare)
        .Help("With a time of day set, hides the game's own lens flare: it stays where the game's "
              "sun is, so at night it would glare out of the dark sky."),
    Slider(1, "SHADOWS",         "Shadows",              &Config::shadows,          0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Real-time sun shadows from bikes, riders, trees and track objects. Fades out at night "
              "(the moon casts faint ones).")
        .Col(1),
    Toggle(1, nullptr,           "Detailed shadows",     &Config::shadowMap)
        .Help("Draws bikes, riders and objects again from the sun for full, sharp shadows, even from "
              "things off screen. Off uses cheaper screen-space shadows only."),
    Slider(1, nullptr,           "Shadow distance",      &Config::shadowDistance,   5.0,   200.0, 1.0,    1.0,    0, " m")
        .Help("Detailed shadows: how far around you they reach. Shadows near you stay sharp at any "
              "distance; the far ones get softer the further it goes. Further costs some speed."),
    Choice3(1, nullptr,          "Shadow resolution",    &Config::shadowResolution, "2048", "4096", "8192")
        .Help("Detailed shadows: how sharp they are, near and far. Higher is sharper, at the cost of "
              "graphics memory and speed (8192 is for high-end graphics cards)."),
    Slider(1, nullptr,           "Contact shadows",      &Config::shadowLength,     1.0,   20.0,  0.5,    1.0,    1, " m")
        .Help("Screen-space shadows: how far they reach from what casts them. They add shadows from "
              "the ground's own shape (ruts, jumps)."),
    Slider(1, nullptr,           "Ambient occlusion",    &Config::ambientOcclusion, 0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Soft contact shadows in ruts, corners and under the bikes, where light can't reach."),
    Choice3(1, "ANTI-ALIASING",   "Anti-aliasing",        &Config::antiAliasing, "OFF", "FXAA", "HIGH")
        .Help("Smooths jagged edges (FXAA). HIGH follows long edges further for cleaner lines at "
              "a slightly higher cost."),
    Slider(1, nullptr,           "Sharpen",              &Config::sharpen,          0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Brings back fine detail, more where the picture is soft. Pairs well with anti-aliasing."),
    Slider(1, "TEXTURES",        "Anisotropic filter",   &Config::anisotropy,       1.0,   16.0,  1.0,    1.0,    0, "x", "Off")
        .Help("Keeps textures sharp when you look along them at a low angle: the track ahead, "
              "the ruts in the distance. 16x is the sharpest."),
    Toggle(1, nullptr,           "Trilinear filtering",  &Config::trilinear)
        .Help("Blends between texture detail levels, so there are no visible lines where the "
              "ground's texture changes sharpness."),
    Slider(1, nullptr,           "Texture LOD bias",     &Config::textureLodBias,   -2.0,  0.0,   0.05,   1.0,    2, "")
        .Help("Below 0 uses sharper texture detail further away. Too low shimmers in motion; "
              "-0.5 is a good start with 16x anisotropic."),
    Slider(1, "GROUND & TIRES",  "Dirt detail",          &Config::dirtDetail,       0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Adds fine high-resolution grain, pebbles and clods to the track surface up close, "
              "lit by the sun. Only on the ground, never on bikes or riders.")
        .Col(2),
    Slider(1, nullptr,           "Detail size",          &Config::dirtScale,        0.5,   2.0,   0.05,   1.0,    2, "x")
        .Help("How big the dirt detail's grains and pebbles are."),
    Slider(1, nullptr,           "Tire marks",           &Config::tireMarks,        0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Your tires dig ruts in the dirt behind you: packed, darker dirt with the knobs' "
              "imprint and loose clods along the edges. They stay on the track for about 160 m "
              "around you, and come back exactly as you made them whenever you return to a spot "
              "you'd ridden (see the mark cache below for how far back that reaches)."),
    Slider(1, nullptr,           "Mark cache",           &Config::markCacheSegs,    10000.0, 1000000.0, 25000.0, 1.0, 0, "", nullptr)
        .Help("How far back along your ride the tire marks are remembered. "
              "A cache of 40 000 is ~11 minutes of riding at 60fps"
              "The cache size doesn't effect FPS, it only changes how much memory is used"),
    Slider(1, nullptr,           "Tire dirt",            &Config::tireDirt,         0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Mud caked on your tires, in clumps that turn with the wheels, lit by the sun."),
    Slider(1, "COLOR",           "Brightness",           &Config::exposure,         -2.0,  2.0,   0.05,   1.0,    2, " EV")
        .Help("Overall exposure. +1 EV is twice as bright, -1 EV half."),
    Slider(1, nullptr,           "Contrast",             &Config::contrast,         -0.5,  0.5,   0.01,   100.0,  0, "%")
        .Help("More (+) or less (-) difference between dark and bright."),
    Slider(1, nullptr,           "Saturation",           &Config::saturation,       -1.0,  1.0,   0.01,   100.0,  0, "%")
        .Help("More (+) or less (-) colourful. -100% is black and white."),
    Slider(1, nullptr,           "Vignette",             &Config::vignette,         0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Darkens the corners of the picture, like a camera lens."),
    Slider(1, "CAMERA EFFECTS",  "Motion blur",          &Config::motionBlur,       0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Blurs the picture with the camera's movement for a sense of speed. Your own bike "
              "stays sharp."),
    Slider(1, nullptr,           "Depth of field",       &Config::depthOfField,     0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Blurs what's out of focus: the bars and the far distance when focused down the "
              "track."),
    Slider(1, nullptr,           "Focus distance",       &Config::dofFocus,         0.0,   100.0, 0.5,    1.0,    1, " m", "Auto")
        .Help("Where depth of field focuses. Auto focuses on the middle of the screen, easing "
              "between distances like an eye."),
    // ======== REPLAY ========
    Choice3(2, "REPLAY CAMERA",   "Camera",               &Config::replayCamera, "GAME", "3RD PERSON", "HELMET")
        .Help("GAME is the replay's own camera, which faces one way while the rider swivels in front "
              "of it. 3RD PERSON locks the camera behind the rider. HELMET is a GoPro on the rider's "
              "helmet, on top or on its side.")
        .Col(0),
    Slider(2, "3RD PERSON CAMERA", "Distance behind",      &Config::replayDistance,   0.5,   12.0,  0.1,    1.0,    1, " m")
        .Help("3rd person replays: how far behind the rider the camera sits. Lower moves it forward, "
              "closer to the rider."),
    Slider(2, nullptr,           "Height",               &Config::replayHeight,     -0.5,  5.0,   0.05,   1.0,    2, " m")
        .Help("3rd person replays: how high above the rider the camera sits."),
    Slider(2, nullptr,           "Pitch",                &Config::replayPitchDeg,   -10.0, 45.0,  0.5,    1.0,    1, "\xB0")
        .Help("3rd person replays: how far the camera looks down. Negative looks up."),
    Choice4(2, "HELMET CAMERA",   "Mount",                &Config::helmetMount, "TOP", "LEFT", "RIGHT", "CHEST")
        .Help("Helmet replays: TOP mounts the camera on top of the helmet, LEFT and RIGHT on its side, "
              "CHEST on the rider's chest (it turns with the body, not the head). Each keeps its own "
              "height, forward and tilt.")
        .Col(1),
    Slider(2, nullptr,           "Mount height",         &Config::helmetHeight,     -1.5,  2.0,   0.01,   1.0,    2, " m")
        .Side(&Config::sideHeight).Chest(&Config::chestHeight)
        .Help("Helmet replays: how far above the rider's eyes the camera sits (chest mount: above the "
              "middle of the shoulders, negative is below them)."),
    Slider(2, nullptr,           "Mount forward",        &Config::helmetForward,    -1.5,  1.5,   0.01,   1.0,    2, " m")
        .Side(&Config::sideForward).Chest(&Config::chestForward)
        .Help("Helmet replays: moves the camera forward (+) or back (-) along the helmet (chest mount: "
              "how far in front of the shoulders it sits)."),
    Slider(2, nullptr,           "Mount out",            &Config::sideOut,          0.0,   1.5,   0.01,   1.0,    2, " m")
        .Help("Side-mounted helmet replays: how far out to the side of the head the camera sits. Raise "
              "it if the camera ends up inside the helmet."),
    Slider(2, nullptr,           "Tilt",                 &Config::helmetTiltDeg,    -10.0, 60.0,  0.5,    1.0,    1, "\xB0")
        .Side(&Config::sideTiltDeg).Chest(&Config::chestTiltDeg)
        .Help("Helmet replays: how far the camera is angled down from where the helmet faces. More "
              "shows more of the bars and the bike, less shows more of the track ahead."),
    Slider(2, nullptr,           "Stabilization",        &Config::helmetSmoothing,  0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Helmet replays: smooths the camera's rotation, like a GoPro's stabilization. Off shakes "
              "with every jolt of the rider's head."),
    Slider(2, nullptr,           "Horizon lock",         &Config::helmetLevel,      0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Helmet replays: how much of the rider's lean is taken out of the view. 0% leans with "
              "the head, 100% keeps the horizon level."),
    // ======== KEYBINDS ========
    KeyRow(3, "KEYBINDS",       "Open / close this menu",    &Config::menuKey)
        .Help("Opens and closes this menu. Click the key, then press the new one. Esc cancels."),
    KeyRow(3, nullptr,          "First person mod on / off", &Config::toggleKey)
        .Help("Turns the first-person camera mod on and off in game (the FIRST PERSON tab's First "
              "person mod switch). Click the key, then press the new one. Esc cancels."),
    KeyRow(3, nullptr,          "Shaders on / off",          &Config::shaderToggleKey)
        .Help("Turns all the shaders on and off in game, for before / after comparisons (the GRAPHICS "
              "tab's Shaders switch). Click the key, then press the new one. Esc cancels."),
    PadRow(3, "CONTROLLER",     "Look left",                 &Config::lookLeftControl)
        .Help("Hold to look left (FIRST PERSON tab: Controller look, Side look angle). Click it, then "
              "press any button, trigger or paddle on your controller, or push a stick the way you want. "
              "Backspace clears it, Esc cancels."),
    PadRow(3, nullptr,          "Look right",                &Config::lookRightControl)
        .Help("Hold to look right. Click it, then press any button, trigger or paddle on your controller, "
              "or push a stick the way you want. Backspace clears it, Esc cancels."),
    PadRow(3, nullptr,          "Look back",                 &Config::lookBackControl)
        .Help("Hold to look behind you (it wins over left and right). Click it, then press any button, "
              "trigger or paddle on your controller, or push a stick the way you want. Backspace clears "
              "it, Esc cancels. Paddles: an Xbox Elite controller's paddles work when they aren't mapped "
              "to another button in the Xbox Accessories app; other controllers' paddles work when they "
              "show up as buttons of their own."),
    // ======== OTHER ========
    Toggle(4, "WHILE RIDING",   "Hide HUD",                  &Config::hideHud)
        .Help("Hides the game's HUD (timing, positions, chat) while you ride, spectate or watch a "
              "replay, for a clean view. Open the game's menu (Esc) and it's all there as usual; "
              "close it and the HUD goes again. Never hidden in the game's menus.")
        .Col(0),
    Toggle(4, nullptr,          "Hide cursor",               &Config::hideCursor)
        .Help("Hides the game's mouse cursor while you ride or watch a replay. It comes back as soon "
              "as you move the mouse, and is never hidden in the game's menus."),
    Toggle(4, "WINDOW",         "Fullscreen window",         &Config::fullscreenWindow)
        .Help("With the game in windowed mode, makes its window cover the whole screen with no border "
              "(borderless fullscreen): Alt+Tab stays instant. Off puts the window back as it was. "
              "Shared by all the presets."),
    Toggle(4, "THIS MENU",      "Fade while adjusting",      &Config::fadeMenu)
        .Help("While you drag a GRAPHICS slider (or step it with the arrow keys) the menu fades out, "
              "all but that slider, so you can see the whole picture change."),
    Toggle(4, "SOUND MODS",     "125 two-stroke",            &Config::soundPack125)
        .Help("A fuller engine sound for every bike that uses the game's stock 125 two-stroke engine. "
              "The stock sounds repeat one recorded engine cycle over and over; this one plays 16 "
              "cycles in a random order, each firing a little differently, like a real engine. "
              "Heard from the next time the game starts.")
        .Col(1),
    Toggle(4, nullptr,          "250 two-stroke",            &Config::soundPack250)
        .Help("A fuller engine sound for every bike that uses the game's stock 250 two-stroke engine "
              "(16 varied cycles instead of one repeated). Heard from the next time the game starts."),
    Toggle(4, nullptr,          "250 four-stroke",           &Config::soundPack250f)
        .Help("A fuller engine sound for every bike that uses the game's stock 250 four-stroke engine "
              "(16 varied cycles instead of one repeated). Heard from the next time the game starts."),
    Toggle(4, nullptr,          "450 four-stroke",           &Config::soundPack450f)
        .Help("A fuller engine sound for every bike that uses the game's stock 450 four-stroke engine "
              "(16 varied cycles instead of one repeated). Heard from the next time the game starts. "
              "Bikes with their own custom sounds keep them."),
};
constexpr int kRowCount = static_cast<int>(sizeof kRows / sizeof kRows[0]);

// Keyboard focus order: the tab bar, the on/off switch, the current tab's rows, then the buttons.
constexpr int kTabBarItem = 0;
constexpr int kFirstRowItem = 1;

struct PanelState {
    bool   open         = false;
    int    tab          = 0;
    int    selected     = kFirstRowItem;
    int    dragging     = -1;  // index into kRows of the slider being dragged with the mouse
    int    pressedItem  = -1;  // button the mouse went down on
    int    listening    = -1;  // index into kRows of the key row waiting for a key press
    double listenEnded  = -1;  // when it last stopped waiting (its key may still be held)
    bool   helpForMouse = false;  // the help box follows the mouse (true) or the selection
    float  mouseX = -1, mouseY = -1;
    bool   dirty        = false;
    bool   closeRequest = false;
    char   status[96]   = "";
    double statusUntil  = 0;
    // Fading while a GRAPHICS slider is adjusted (Config::fadeMenu): all but that slider.
    float  opacity      = 1.0f;
    double lastFrame    = 0;
    int    adjustRow    = -1;   // index into kRows of the slider last adjusted
    double adjustedAt   = -1;   // when (by the keys; dragging counts for as long as it lasts)
};
PanelState g_panel;

// Parts of the batch the fade leaves alone (the slider being adjusted, the cursor): [begin, end).
struct KeepRange { size_t begin, end; };
std::vector<KeepRange> g_keepRanges;
constexpr float  kFadedOpacity = 0.06f;
constexpr double kAdjustHold   = 0.6;  // s the menu stays faded after the last arrow key step

// ---- The game's HUD and cursor while riding (Config::hideHud / hideCursor) -----------------
// The cursor: activity is the mouse moving or a button / Esc going down; it shows it for a few
// seconds. The HUD: hidden all the while you ride, shown with the game's menu (GameMenuOverScene),
// which Esc opens: Esc presses to the game are counted for it (TakeGameEscapes).
constexpr ULONGLONG kRevealMs = 3000;
std::atomic<ULONGLONG> g_lastMouseActivity{ 0 };
std::atomic<int>       g_gameEscapes{ 0 };       // Esc presses that reached the game
std::atomic<bool>      g_keyMessages{ false };   // the game gets its keys as window messages
std::atomic<long long> g_lastMousePos{ 0 };   // (x << 32) | y, screen pixels
std::atomic<HWND>      g_gameWindow{ nullptr };

// ---- Sound mods (Config::soundPack*) ----------------------------------------------------------
// A switch turned in the panel installs or removes its pack right away (see sound_packs.h).
void SetStatus(const char* text, double now);

void UpdateSoundPacks(double now)
{
    Config& cfg = EditConfig();
    bool* const switches[soundpacks::kCount] = { &cfg.soundPack125, &cfg.soundPack250, &cfg.soundPack250f, &cfg.soundPack450f };
    for (int i = 0; i < soundpacks::kCount; ++i) {
        if (*switches[i] == soundpacks::Installed(i))
            continue;
        SetStatus(soundpacks::Set(i, *switches[i]), now);
        *switches[i] = soundpacks::Installed(i);  // (back off if it couldn't be installed)
    }
}

// ---- Fullscreen window (Config::fullscreenWindow) --------------------------------------------
// The game's window in windowed mode made borderless and stretched over its monitor; what it was
// (style, extended style, placement) is kept to put it back when the option is turned off. A
// window already bare and covering its monitor (the game's own fullscreen) is left alone.
struct FullscreenWindow {
    HWND            window = nullptr;   // the window made fullscreen (null: none)
    LONG_PTR        style = 0, exStyle = 0;
    WINDOWPLACEMENT placement{};
    ULONGLONG       lastTry = 0;
};
FullscreenWindow g_fullscreen;

constexpr LONG_PTR kFrameStyles   = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
constexpr LONG_PTR kFrameExStyles = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;

void UpdateFullscreenWindow(HWND window, bool wanted)
{
    FullscreenWindow& f = g_fullscreen;
    if (!window || (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD))
        return;  // drawn into a window inside another: not one to stretch
    if (f.window && (f.window != window || !IsWindow(f.window)))
        f.window = nullptr;  // the game made a new window: the old one's state is gone with it
    if (!wanted) {
        if (!f.window)
            return;
        // Back as it was: its frame, then its place and size.
        SetWindowLongPtrW(window, GWL_STYLE, f.style);
        SetWindowLongPtrW(window, GWL_EXSTYLE, f.exStyle);
        SetWindowPlacement(window, &f.placement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
        f.window = nullptr;
        Log("window: back to a normal window");
        return;
    }
    if (IsIconic(window))
        return;  // minimised: nothing to fill
    MONITORINFO monitor{};
    monitor.cbSize = sizeof monitor;
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
        return;
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    RECT rect;
    if (!GetWindowRect(window, &rect))
        return;
    // One pixel taller than the screen (the extra row is off its bottom edge): a window exactly the
    // size of the screen is taken for real fullscreen and no longer synced to the screen by Windows,
    // which tears the picture into bands whenever the game runs without vsync. The taskbar still
    // goes behind it.
    const RECT& m = monitor.rcMonitor;
    const RECT target = { m.left, m.top, m.right, m.bottom + 1 };
    const bool bare = !(style & (WS_CAPTION | WS_THICKFRAME));
    if (bare && (EqualRect(&rect, &target) || EqualRect(&rect, &m)))
        return;  // already fullscreen (ours, or the game's own)
    // At most once a second, so a window that won't take it isn't fought every frame.
    const ULONGLONG now = GetTickCount64();
    if (now - f.lastTry < 1000)
        return;
    f.lastTry = now;
    if (!f.window) {
        f.window = window;
        f.style = style;
        f.exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        f.placement.length = sizeof f.placement;
        GetWindowPlacement(window, &f.placement);
        Log("window: fullscreen window (%ldx%ld, borderless, one pixel taller than the screen so it stays in sync with it)",
            monitor.rcMonitor.right - monitor.rcMonitor.left,
            monitor.rcMonitor.bottom - monitor.rcMonitor.top);
    }
    if (IsZoomed(window))
        ShowWindow(window, SW_RESTORE);  // a maximised window keeps its own size
    SetWindowLongPtrW(window, GWL_STYLE, style & ~kFrameStyles);
    SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) & ~kFrameExStyles);
    SetWindowPos(window, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top,
                 SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
}

void NoteMouseActivity()
{
    // For automated tests: the real mouse (whoever is at the desk) doesn't count.
    static const bool testIdle = GetEnvironmentVariableA("MXCAM_TEST_MOUSE_IDLE", nullptr, 0) > 0;
    if (testIdle)
        return;
    POINT p;
    if (!GetCursorPos(&p))
        return;
    const long long packed = (static_cast<long long>(p.x) << 32) | static_cast<unsigned int>(p.y);
    const bool moved = g_lastMousePos.exchange(packed) != packed;
    const bool pressed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
                         (GetAsyncKeyState(VK_MBUTTON) & 0x8000) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
    if (moved || pressed)
        g_lastMouseActivity = GetTickCount64();
}

struct Input {
    float mx = -1, my = -1;
    bool  inside = false, down = false, pressed = false, released = false;
    int   nav = 0;   // -1 up, +1 down
    int   step = 0;  // -1 left, +1 right
    bool  activate = false, coarse = false;

    bool Inside(float x, float y, float w, float h) const
    {
        return inside && mx >= x && mx < x + w && my >= y && my < y + h;
    }
};

struct KeyState {
    bool   down = false;
    double nextRepeat = 0;
};
struct Keys {
    KeyState menu, escape, up, downArrow, left, right, enter, space, mouse;
};
Keys g_keys;

// Edge-triggered key poll; with repeat, a held key fires again after 0.35 s, then every 45 ms.
bool PollKey(KeyState& key, int vk, bool repeat, double now)
{
    const bool down = vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool fire = false;
    if (down && !key.down) {
        fire = true;
        key.nextRepeat = now + 0.35;
    } else if (down && repeat && now >= key.nextRepeat) {
        fire = true;
        key.nextRepeat = now + 0.045;
    }
    key.down = down;
    return fire;
}

double Seconds()
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
}

std::atomic<bool> g_menuOpen{ false };  // read by the window procedure (the window's thread)
HWND    g_hookedWindow   = nullptr;
WNDPROC g_previousProc   = nullptr;
bool    g_windowUnicode  = true;

LRESULT CALLBACK PanelWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    // Esc going down for the game (not held, not to this panel, which keeps presses to itself): the
    // game's menu opens or closes (Config::hideHud shows the HUD with it).
    if (message == WM_KEYDOWN) {
        g_keyMessages = true;
        if (wParam == VK_ESCAPE && !(lParam & (1 << 30)) && !g_menuOpen.load(std::memory_order_relaxed))
            ++g_gameEscapes;
    }
    // The game's cursor while riding (Config::hideCursor), if it's the system's: none over the
    // game while the mouse is left alone. A move sends this first, and shows the game's again.
    if (message == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT && !g_menuOpen.load(std::memory_order_relaxed)) {
        NoteMouseActivity();
        if (HideGameOverlayNow(true)) {
            SetCursor(nullptr);
            return TRUE;
        }
    }
    if (g_menuOpen.load(std::memory_order_relaxed)) {
        switch (message) {
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(nullptr);  // the panel draws its own cursor
                return TRUE;
            }
            break;
        // Presses go to the panel only. Releases (WM_KEYUP, WM_*BUTTONUP) still reach the game so
        // nothing it saw go down can get stuck; Alt combinations (WM_SYSKEYDOWN) pass too.
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_KEYDOWN:
        case WM_CHAR:
            return 0;
        default:
            break;
        }
    }
    return g_windowUnicode ? CallWindowProcW(g_previousProc, window, message, wParam, lParam)
                           : CallWindowProcA(g_previousProc, window, message, wParam, lParam);
}

void SubclassWindow(HWND window)
{
    if (window == g_hookedWindow)
        return;
    // A recreated window (e.g. a fullscreen switch): give the old one its procedure back.
    if (g_hookedWindow && IsWindow(g_hookedWindow) &&
        GetWindowLongPtrW(g_hookedWindow, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&PanelWindowProc)) {
        SetWindowLongPtrW(g_hookedWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_previousProc));
    }
    g_windowUnicode = IsWindowUnicode(window) != FALSE;
    // Read the old procedure first so a message arriving mid-swap can always be forwarded.
    g_previousProc = reinterpret_cast<WNDPROC>(g_windowUnicode ? GetWindowLongPtrW(window, GWLP_WNDPROC)
                                                               : GetWindowLongPtrA(window, GWLP_WNDPROC));
    if (g_windowUnicode)
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PanelWindowProc));
    else
        SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PanelWindowProc));
    g_hookedWindow = window;
}

void OpenPanel(HWND window)
{
    SubclassWindow(window);
    g_panel.open = true;
    g_panel.dragging = -1;
    g_panel.pressedItem = -1;
    g_panel.listening = -1;
    g_panel.closeRequest = false;
    g_menuOpen = true;
    Log("panel: opened");
}

void ClosePanel()
{
    g_panel.open = false;
    g_panel.dragging = -1;
    g_panel.listening = -1;
    g_menuOpen = false;
    Log("panel: closed%s", g_panel.dirty ? " (unsaved changes kept for this session)" : "");
}

Input ReadInput(HWND window, bool focus, int width, int height, double now)
{
    Input in;
    const bool up    = PollKey(g_keys.up, VK_UP, true, now);
    const bool down  = PollKey(g_keys.downArrow, VK_DOWN, true, now);
    const bool left  = PollKey(g_keys.left, VK_LEFT, true, now);
    const bool right = PollKey(g_keys.right, VK_RIGHT, true, now);
    const bool enter = PollKey(g_keys.enter, VK_RETURN, false, now);
    const bool space = PollKey(g_keys.space, VK_SPACE, false, now);
    const bool wasDown = g_keys.mouse.down;
    PollKey(g_keys.mouse, VK_LBUTTON, false, now);
    if (!focus)
        return in;

    POINT cursor;
    if (GetCursorPos(&cursor) && ScreenToClient(window, &cursor)) {
        in.mx = static_cast<float>(cursor.x);
        in.my = static_cast<float>(cursor.y);
        in.inside = cursor.x >= 0 && cursor.y >= 0 && cursor.x < width && cursor.y < height;
    }
    in.down     = g_keys.mouse.down;
    in.pressed  = in.down && !wasDown && in.inside;
    in.released = !in.down && wasDown;
    in.nav      = (down ? 1 : 0) - (up ? 1 : 0);
    in.step     = (right ? 1 : 0) - (left ? 1 : 0);
    in.activate = enter || space;
    in.coarse   = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    return in;
}

const char* KeyName(int vk)
{
    static char name[32];
    const UINT scanCode = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    if (!scanCode || GetKeyNameTextA(static_cast<LONG>(scanCode << 16), name, sizeof name) <= 0)
        snprintf(name, sizeof name, "KEY %02X", vk);
    for (char* c = name; *c; ++c)
        *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));
    return name;
}

void SetStatus(const char* text, double now)
{
    snprintf(g_panel.status, sizeof g_panel.status, "%s", text);
    g_panel.statusUntil = now + 2.5;
}

// The first keyboard key to go down since the last call, or 0. Mouse buttons don't count.
int PollNewKey()
{
    static bool wasDown[256];
    int pressed = 0;
    for (int vk = VK_BACK; vk < 0xFF; ++vk) {
        if (vk >= VK_LSHIFT && vk <= VK_RMENU)  // reported as plain Shift / Ctrl / Alt as well
            continue;
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !wasDown[vk] && !pressed)
            pressed = vk;
        wasDown[vk] = down;
    }
    return pressed;
}

void StartListening(int row)
{
    g_panel.listening = row;
    PollNewKey();  // keys already held (the Enter that got here) don't count as the answer
    if (kRows[row].kind == RowKind::Pad)
        gamepad::StartCapture();  // nor do controls already held
}


void Upper(const char* text, char* out, size_t size)
{
    size_t i = 0;
    for (; text[i] && i + 1 < size; ++i)
        out[i] = static_cast<char>(toupper(static_cast<unsigned char>(text[i])));
    out[i] = '\0';
}

// Keys the panel steers with, or that another hotkey already has, can't be taken.
const char* KeyConflict(int vk, int Config::* field, const Config& cfg)
{
    switch (vk) {
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_RETURN: case VK_SPACE: case VK_SHIFT:
        return "USED BY THE MENU";
    default:
        break;
    }
    const struct { int Config::* key; const char* what; } bound[] = {
        { &Config::menuKey,         "THE MENU KEY" },
        { &Config::toggleKey,       "THE FIRST PERSON MOD KEY" },
        { &Config::shaderToggleKey, "THE SHADERS KEY" },
        { &Config::reloadKey,       "THE RELOAD KEY (MXCAM.INI)" },
        { &Config::dumpKey,         "THE FRAME DUMP KEY (MXCAM.INI)" },
        { &Config::traceKey,        "THE TRACE KEY (MXCAM.INI)" },
    };
    for (const auto& b : bound)
        if (b.key != field && cfg.*b.key == vk)
            return b.what;
    return nullptr;
}

// A controller row is waiting: a control pressed on the controller is taken (unless another look
// has it); on the keyboard, Backspace / Delete clears the bind and Esc cancels.
void ListenForPad(double now)
{
    const RowDef& def = kRows[g_panel.listening];
    const int vk = PollNewKey();
    gamepad::Control control = gamepad::PollCapture();
    if (!vk && control == gamepad::kNone)
        return;
    if (vk && vk != VK_BACK && vk != VK_DELETE && vk != VK_ESCAPE && control == gamepad::kNone)
        return;  // other keys: still waiting for the controller
    g_panel.listening = -1;
    g_panel.listenEnded = now;
    if (vk == VK_ESCAPE)
        return;
    if (vk == VK_BACK || vk == VK_DELETE)
        control = gamepad::kNone;

    Config& cfg = EditConfig();
    char text[128], label[64];
    Upper(def.label, label, sizeof label);
    const struct { int Config::* field; const char* what; } looks[] = {
        { &Config::lookLeftControl,  "LOOK LEFT" },
        { &Config::lookRightControl, "LOOK RIGHT" },
        { &Config::lookBackControl,  "LOOK BACK" },
    };
    for (const auto& l : looks) {
        if (control != gamepad::kNone && l.field != def.pad && cfg.*l.field == control) {
            snprintf(text, sizeof text, "%s IS ALREADY %s", gamepad::ControlName(control), l.what);
            SetStatus(text, now);
            return;
        }
    }
    int& bound = cfg.*def.pad;
    if (bound != control) {
        bound = control;
        g_panel.dirty = true;
    }
    snprintf(text, sizeof text, "%s: %s - NOT SAVED YET", label, gamepad::ControlName(control));
    SetStatus(text, now);
}

// A key row is waiting: Esc cancels, any other key is taken (unless it clashes).
void ListenForKey(double now)
{
    if (kRows[g_panel.listening].tab != g_panel.tab) {  // switched tabs with the mouse
        g_panel.listening = -1;
        return;
    }
    if (kRows[g_panel.listening].kind == RowKind::Pad) {
        ListenForPad(now);
        return;
    }
    const int vk = PollNewKey();
    if (!vk)
        return;
    const RowDef& def = kRows[g_panel.listening];
    g_panel.listening = -1;
    g_panel.listenEnded = now;
    if (vk == VK_ESCAPE)
        return;

    Config& cfg = EditConfig();
    char text[128], label[64];
    Upper(def.label, label, sizeof label);
    if (const char* conflict = KeyConflict(vk, def.key, cfg)) {
        snprintf(text, sizeof text, "%s IS ALREADY %s", KeyName(vk), conflict);
        SetStatus(text, now);
        return;
    }
    int& key = cfg.*def.key;
    if (key != vk) {
        key = vk;
        g_panel.dirty = true;
    }
    g_keys.menu.down = true;  // the press that picked the key must not also close the panel
    snprintf(text, sizeof text, "%s: %s - NOT SAVED YET", label, KeyName(vk));
    SetStatus(text, now);
}

// ===========================================================================
// The panel (immediate mode: layout, input and drawing in one pass)
// ===========================================================================
void FormatValue(const RowDef& def, double value, char* out, size_t size)
{
    if (def.number == &Config::markCacheSegs) {
        // The mark cache: the count it keeps, and the memory it costs (both wheels, kMarkSegBytes
        // a segment), so the size can be weighed before it's set.
        const double bytes = value * 2.0 * static_cast<double>(kMarkSegBytes);
        snprintf(out, size, "%.0f segs: %.1f MB", value, bytes / (1024.0 * 1024.0));
        return;
    }
    if (def.zeroText && value <= def.min + def.step * 0.5) {
        snprintf(out, size, "%s", def.zeroText);
    } else if (def.clock) {
        const int minutes = static_cast<int>(std::lround(value * 60.0)) % (24 * 60);
        snprintf(out, size, "%02d:%02d", minutes / 60, minutes % 60);
    } else if (def.min < 0)
        snprintf(out, size, "%+.*f%s", def.decimals, value * def.display, def.unit);
    else
        snprintf(out, size, "%.*f%s", def.decimals, value * def.display, def.unit);
}

void SetSlider(const RowDef& def, double& value, double wanted)
{
    const double snapped = def.min + std::round((wanted - def.min) / def.step) * def.step;
    const double clamped = Clamp(snapped, def.min, def.max);
    if (clamped != value) {
        value = clamped;
        g_panel.dirty = true;
    }
}

void SetFlag(bool& flag, bool wanted)
{
    if (flag != wanted) {
        flag = wanted;
        g_panel.dirty = true;
    }
}

void SetOption(int& option, int wanted)
{
    if (option != wanted) {
        option = wanted;
        g_panel.dirty = true;
    }
}

// Rows that currently have no effect are drawn dimmed (they can still be adjusted).
bool RowInactive(const RowDef& def, const Config& cfg)
{
    if (def.locked || (def.customOnly && !cfg.customCamera))
        return true;
    if ((def.tab == kTabFirstPerson || def.tab == kTabReplay) && !cfg.enabled && def.flag != &Config::enabled)
        return true;  // the first person mod is off (it does the replay cameras too)
    if (def.tab == kTabReplay && def.number) {  // placement for a replay camera that isn't the one chosen
        const bool chase = def.number == &Config::replayDistance || def.number == &Config::replayHeight ||
                           def.number == &Config::replayPitchDeg;
        if (def.number == &Config::sideOut && cfg.helmetMount != 1 && cfg.helmetMount != 2)
            return true;
        return cfg.replayCamera != (chase ? 1 : 2);
    }
    if (def.option == &Config::helmetMount)
        return cfg.replayCamera != 2;
    if (def.tab == kTabGraphics) {  // the shaders' rows: off with the master switch, or what they refine
        if (def.flag == &Config::shaders)
            return false;
        if (!cfg.shaders)
            return true;
        if (def.flag == &Config::moveSun || def.flag == &Config::hideGameFlare || def.number == &Config::nightBrightness)
            return cfg.timeOfDay < 0;
        if (def.number == &Config::shadowLength)  // (screen-space: only without the shadow map)
            return cfg.shadows <= 0 || cfg.shadowMap;
        if (def.flag == &Config::shadowMap)
            return cfg.shadows <= 0;
        if (def.number == &Config::shadowDistance || def.option == &Config::shadowResolution)
            return cfg.shadows <= 0 || !cfg.shadowMap;
        if (def.number == &Config::headlight)
            return cfg.timeOfDay < 0;
        if (def.number == &Config::headlightBeamDeg || def.number == &Config::headlightAimDeg)
            return cfg.timeOfDay < 0 || cfg.headlight <= 0;
        if (def.number == &Config::dirtScale)
            return cfg.dirtDetail <= 0;
        if (def.number == &Config::markCacheSegs)
            return cfg.tireMarks <= 0;
        if (def.number == &Config::dofFocus)
            return cfg.depthOfField <= 0;
    }
    return false;
}

// A tab's columns: how many, and the tallest (the panel is sized for the tallest of all the tabs,
// so it doesn't jump around).
constexpr int kMaxColumns = 3;
struct TabLayout {
    int   columns = 1;
    float height  = 0;
};
TabLayout LayoutTab(int tab, float sectionH, float rowH)
{
    TabLayout layout;
    float columnH[kMaxColumns] = {};
    int column = 0;
    for (const RowDef& def : kRows) {
        if (def.tab != tab)
            continue;
        if (def.column >= 0)
            column = Clamp(def.column, 0, kMaxColumns - 1);
        layout.columns = column + 1 > layout.columns ? column + 1 : layout.columns;
        columnH[column] += (def.section ? sectionH : 0) + rowH;
    }
    for (float h : columnH)
        layout.height = std::fmax(layout.height, h);
    return layout;
}

void BuildPanel(int screenWidth, int screenHeight, const Input& in, double now)
{
    Config& cfg = EditConfig();
    const float s = g_atlasScale;
    const auto S = [s](float v) { return std::round(v * s); };

    int rowsInTab = 0;
    for (const RowDef& def : kRows)
        rowsInTab += def.tab == g_panel.tab ? 1 : 0;
    const int presetItem = kFirstRowItem + rowsInTab;
    const int saveItem = presetItem + 1, defaultsItem = saveItem + 1, closeItem = saveItem + 2;
    const int itemCount = closeItem + 1;
    if (g_panel.selected >= itemCount)
        g_panel.selected = itemCount - 1;
    if (in.nav)
        g_panel.selected = (g_panel.selected + in.nav + itemCount) % itemCount;

    // Help follows whichever was used last: the mouse (what's under it) or the keys (the selection).
    if (in.inside && (in.mx != g_panel.mouseX || in.my != g_panel.mouseY || in.pressed))
        g_panel.helpForMouse = true;
    if (in.nav || in.step || in.activate)
        g_panel.helpForMouse = false;
    g_panel.mouseX = in.mx;
    g_panel.mouseY = in.my;
    // Where the help box goes: beside the panel, level with the row (or under it in the row's own
    // column, when the screen is too narrow for the box beside the panel).
    struct HelpSpot { const char* text = nullptr; float top = 0, bottom = 0, left = 0, width = 0; };
    HelpSpot hoveredHelp, selectedHelp;
    const auto OfferHelp = [&](const char* text, float top, float bottom, float left, float width, bool hot, bool selected) {
        if (hot)
            hoveredHelp = { text, top, bottom, left, width };
        if (selected)
            selectedHelp = { text, top, bottom, left, width };
    };

    // Up to three columns of rows side by side.
    const float pad = S(26), columnW = S(372), columnGap = S(36);
    const float innerW = kMaxColumns * columnW + (kMaxColumns - 1) * columnGap, panelW = innerW + 2 * pad;
    const float headerH = S(56), tabsH = S(46), sectionH = S(38), rowH = S(50), buttonH = S(42);
    float contentH = 0;
    for (int tab = 0; tab < kTabCount; ++tab)
        contentH = std::fmax(contentH, LayoutTab(tab, sectionH, rowH).height);
    const float panelH = pad + headerH + tabsH + contentH + S(24) + S(36) + buttonH + S(14) + S(18) + S(20);
    const float px = S(32);
    const float py = std::fmax(S(16), std::round((screenHeight - panelH) * 0.5f));

    // Keyboard-focus marker: a short blue bar just left of the focused item.
    const auto Marker = [&](float left, float top, float height) { Rect(left, top, S(3), height, kBlueHot); };

    // A panel: navy-charcoal, a blue glow rising from the bottom, a royal blue outline.
    const auto PanelFill = [&](float bx, float by, float bw, float bh, float r) {
        RoundedRect(bx - S(2), by - S(2), bw + S(4), bh + S(4), r + S(2), kBorder);
        RoundedRect(bx, by, bw, bh, r, kPanelColor);
        const int bands = 24;
        const float top = by + bh * 0.35f, bottom = by + bh - r, step = (bottom - top) / bands;
        for (int b = 0; b < bands; ++b) {
            const float t = (b + 1.0f) / bands;
            Rect(bx, std::round(top + b * step), bw, std::ceil(step) + 1, Rgba(24, 52, 120, 0.03f + 0.07f * t));
        }
    };
    PanelFill(px, py, panelW, panelH, S(12));

    // ---- Header: title and the on/off switch ----
    const float x = px + pad;
    float y = py + pad;
    Text(kFontTitle, x, y + S(2), "MXS Remastered", White(1.0f), S(1));

    // A pill switch; returns its outer width.
    const auto Switch = [&](float rightEdge, float centerY, float h, bool on, bool hot) {
        const float w = std::round(h * 1.8f), r = h * 0.5f, sx = rightEdge - w, sy = std::round(centerY - r);
        if (on) {
            RoundedRect(sx, sy, w, h, r, hot ? kBlueHot : kBlue);
            Disc(sx + w - r, sy + r, r - S(4), White(1.0f));
        } else {
            RoundedRect(sx, sy, w, h, r, hot ? kBlue : kLine);
            RoundedRect(sx + 1, sy + 1, w - 2, h - 2, r - 1, kPanelSolid);
            Disc(sx + r, sy + r, r - S(5), Gray(hot ? 200 : 130));
        }
        return w;
    };

    y += headerH;

    // ---- Tabs ----
    {
        if (g_panel.selected == kTabBarItem && in.step)
            g_panel.tab = (g_panel.tab + in.step + kTabCount) % kTabCount;
        // Each tab is its label plus an equal share of the spare width, so long labels still fit
        // (with tighter letter spacing if they would only just fit).
        float labelW[kTabCount], spare = innerW, tracking = S(2);
        for (int pass = 0; pass < 2; ++pass) {
            spare = innerW;
            for (int tab = 0; tab < kTabCount; ++tab) {
                labelW[tab] = TextWidth(kFontSmall, kTabs[tab], tracking);
                spare -= labelW[tab];
            }
            if (spare >= kTabCount * S(8))
                break;
            tracking = S(1) > 0 ? S(1) : 1;
        }
        float tx = x;
        const bool equal = spare >= kTabCount * S(40);  // room for equal tabs
        for (int tab = 0; tab < kTabCount; ++tab) {
            const float tabW = equal ? std::floor(innerW / kTabCount) : labelW[tab] + spare / kTabCount;
            const bool hot = in.Inside(tx, y, tabW, tabsH - S(8));
            if (hot && in.pressed) {
                g_panel.tab = tab;
                g_panel.selected = kTabBarItem;
            }
            const bool current = tab == g_panel.tab;
            const float lx = std::round(tx + (tabW - labelW[tab]) * 0.5f);
            Text(kFontSmall, lx, y + S(8), kTabs[tab], current ? White(1.0f) : (hot ? kLabel : kDim), tracking);
            Rect(tx + S(6), y + S(30), tabW - S(12), current ? S(2) : 1, current ? White(1.0f) : kLine);
            tx += tabW;
        }
        if (g_panel.selected == kTabBarItem)
            Marker(px + S(10), y + S(4), S(26));
        y += tabsH;
    }
    const float rowsTop = y;

    // ---- Rows of the current tab, column by column ----
    const TabLayout layout = LayoutTab(g_panel.tab, sectionH, rowH);
    float colW = std::floor((innerW - (layout.columns - 1) * columnGap) / layout.columns);
    if (layout.columns == 1)
        colW = std::fmin(colW, S(600));  // one short list: not stretched across the whole panel
    for (int c = 1; c < layout.columns; ++c) {  // a faint line between the columns
        const float lineX = std::round(x + c * (colW + columnGap) - columnGap * 0.5f);
        Rect(lineX, rowsTop + S(10), 1, contentH - S(10), kLine);
    }
    int item = kFirstRowItem, column = -1;
    bool firstInColumn = true;
    float cx = x;
    for (int i = 0; i < kRowCount; ++i) {
        const RowDef& def = kRows[i];
        if (def.tab != g_panel.tab)
            continue;
        const int thisItem = item++;
        const int wantColumn = def.column >= 0 ? Clamp(def.column, 0, kMaxColumns - 1) : (column < 0 ? 0 : column);
        if (wantColumn != column) {
            column = wantColumn;
            cx = std::round(x + column * (colW + columnGap));
            y = rowsTop;
            firstInColumn = true;
        }
        const float cw = colW;
        if (def.section) {
            if (!firstInColumn)  // the tab underline already separates the first section
                Rect(cx, y + S(8), cw, 1, kLine);
            Text(kFontSmall, cx, y + S(19), def.section, kSection, S(2));
            y += sectionH;
        }
        firstInColumn = false;

        const bool inactive = RowInactive(def, cfg);
        const uint32_t labelColor = inactive ? kDim : kLabel;
        const uint32_t valueColor = inactive ? kDim : White(1.0f);
        const float hx = cx - S(12), hw = cw + S(24);  // the row's hover area reaches into the gaps
        const bool hot = in.Inside(hx, y, hw, rowH);
        const bool focused = g_panel.selected == thisItem;
        if (hot && in.pressed)
            g_panel.selected = thisItem;
        const bool active = g_panel.dragging == i;
        // The slider being adjusted while the menu fades: kept, on a backdrop of its own.
        const bool kept = i == g_panel.adjustRow && g_panel.opacity < 0.999f;
        const size_t keptBegin = g_batch.size();
        if (kept)
            RoundedRect(hx - S(6), y - S(4), hw + S(12), rowH + S(8), S(10),
                        Rgba(13, 17, 26, 0.9f * (1.0f - g_panel.opacity)));
        if (hot || active)
            RoundedRect(hx, y, hw, rowH, S(6), Rgba(40, 90, 176, 0.14f));
        if (focused)
            Marker(cx - S(17), y + S(9), rowH - S(18));
        OfferHelp(def.help, y, y + rowH, hx, hw, hot, focused);
        const float labelY = y + std::round((rowH - LineHeight(kFontBody)) * 0.5f);

        if (def.kind == RowKind::Slider) {
            double& value = cfg.*SliderField(def, cfg);
            if (hot && in.pressed && !def.locked)
                g_panel.dragging = i;
            if (active && in.down) {
                SetSlider(def, value, def.min + Clamp((in.mx - cx) / cw, 0.0f, 1.0f) * (def.max - def.min));
                g_panel.adjustRow = i;
            }
            if (focused && in.step && !def.locked) {
                SetSlider(def, value, value + in.step * def.step * (in.coarse ? 10 : 1));
                g_panel.adjustRow = i;
                g_panel.adjustedAt = now;
            }

            char text[32];
            FormatValue(def, value, text, sizeof text);
            Text(kFontBody, cx, y + S(5), def.label, labelColor, 0);
            Text(kFontValue, cx + cw - TextWidth(kFontValue, text, 0), y + S(5), text, valueColor, 0);

            const float trackY = y + S(36);
            const float t = static_cast<float>((value - def.min) / (def.max - def.min));
            const float knobX = std::round(cx + t * cw);
            Rect(cx, trackY, cw, S(2), inactive ? kLine : kTrack);
            if (def.min < 0 && !def.clock) {  // signed values fill from the centre
                const float zeroX = std::round(cx + static_cast<float>(-def.min / (def.max - def.min)) * cw);
                Rect(std::fmin(zeroX, knobX), trackY, std::fabs(knobX - zeroX), S(2), inactive ? kDim : White(0.95f));
                Rect(zeroX, trackY - S(3), 1, S(8), kSection);
            } else {
                Rect(cx, trackY, knobX - cx, S(2), inactive ? kDim : White(0.95f));
            }
            if (active || hot)
                Disc(knobX, trackY + S(1), S(11), Rgba(26, 100, 214, 0.35f));
            Disc(knobX, trackY + S(1), active ? S(8) : (hot ? S(7) : S(6)), inactive ? kDim : White(1.0f));
        } else if (def.kind == RowKind::Toggle) {
            bool& flag = cfg.*def.flag;
            if ((hot && in.pressed) || (focused && (in.step || in.activate))) {
                SetFlag(flag, !flag);
                if (def.flag == &Config::enabled)
                    camera::Reset();
            }
            Text(kFontBody, cx, labelY, def.label, labelColor, 0);
            const float sw = Switch(cx + cw, y + rowH * 0.5f, S(22), flag, hot);
            const char* onOff = flag ? "ON" : "OFF";
            Text(kFontSmall, cx + cw - sw - S(10) - TextWidth(kFontSmall, onOff, S(2)),
                 y + std::round((rowH - LineHeight(kFontSmall)) * 0.5f), onOff, flag ? valueColor : kDim, S(2));
        } else if (def.kind == RowKind::Key || def.kind == RowKind::Pad) {  // the key's (control's) name in a box; click it, then press one
            const bool listening = g_panel.listening == i;
            if (listening && in.pressed)  // a second click, here or anywhere else, cancels
                g_panel.listening = -1;
            else if ((hot && in.pressed) || (focused && in.activate))
                StartListening(i);
            const bool waiting = g_panel.listening == i;

            const char* text = def.kind == RowKind::Pad
                ? (waiting ? "PRESS OR PUSH" : gamepad::ControlName(cfg.*def.pad))
                : (waiting ? "PRESS A KEY" : KeyName(cfg.*def.key));
            const float boxH = S(28), boxPad = S(14), boxY = std::round(y + (rowH - boxH) * 0.5f);
            const float textW = TextWidth(kFontSmall, text, S(2));
            const float boxW = std::fmax(textW + 2 * boxPad, S(130)), bx = cx + cw - boxW;
            if (waiting) {
                RoundedRect(bx, boxY, boxW, boxH, boxH * 0.5f, kBlue);
            } else {
                RoundedRect(bx - 1, boxY - 1, boxW + 2, boxH + 2, boxH * 0.5f + 1, hot ? kBlueHot : kBorder);
                RoundedRect(bx, boxY, boxW, boxH, boxH * 0.5f, kPanelSolid);
            }
            Text(kFontSmall, std::round(bx + (boxW - textW) * 0.5f), boxY + std::round((boxH - LineHeight(kFontSmall)) * 0.5f), text,
                 waiting ? White(1.0f) : valueColor, S(2));
            Text(kFontBody, cx, labelY, def.label, labelColor, 0);
        } else {  // Choice: two to four segments, e.g. GAME | CUSTOM
            const char* options[4] = { def.offText, def.onText, def.thirdText, def.fourthText };
            const int count = def.option ? (def.fourthText ? 4 : 3) : 2;
            const int current = def.option ? cfg.*def.option : (cfg.*def.flag ? 1 : 0);
            const auto Pick = [&](int o) {
                if (def.option)
                    SetOption(cfg.*def.option, o);
                else
                    SetFlag(cfg.*def.flag, o == 1);
            };
            const float segH = S(28), segPad = S(14), segY = std::round(y + (rowH - segH) * 0.5f);
            float segW[4], total = 0;
            for (int o = 0; o < count; ++o) {
                segW[o] = TextWidth(kFontSmall, options[o], S(2)) + 2 * segPad;
                total += segW[o];
            }
            float sx = cx + cw - total;
            RoundedRect(sx - 1, segY - 1, total + 2, segH + 2, segH * 0.5f + 1, kBorder);
            RoundedRect(sx, segY, total, segH, segH * 0.5f, kPanelSolid);
            for (int o = 0; o < count; ++o) {
                const bool selected = current == o;
                const bool segHot = in.Inside(sx, segY, segW[o], segH);
                if (segHot && in.pressed)
                    Pick(o);
                if (selected)
                    RoundedRect(sx, segY, segW[o], segH, segH * 0.5f, segHot ? kBlueHot : kBlue);
                Text(kFontSmall, sx + segPad, segY + std::round((segH - LineHeight(kFontSmall)) * 0.5f), options[o],
                     selected ? White(1.0f) : (segHot ? kLabel : kDim), S(2));
                sx += segW[o];
            }
            if (focused && in.step)
                Pick(Clamp(current + (in.step > 0 ? 1 : -1), 0, count - 1));
            if (focused && in.activate)
                Pick((current + 1) % count);
            Text(kFontBody, cx, labelY, def.label, labelColor, 0);
        }
        if (kept)
            g_keepRanges.push_back({ keptBegin, g_batch.size() });
        y += rowH;
    }
    y = rowsTop + contentH;

    // ---- Live readout for the current tab ----
    Rect(x, y + S(10), innerW, 1, kLine);
    y += S(24);
    char readout[256];
    const camera::Telemetry& t = camera::LastTelemetry();
    if (g_panel.tab == kTabGraphics) {
        const char* tires = postfx::TireStatusText();
        snprintf(readout, sizeof readout, "%s   \xB7   %s", postfx::StatusText(), tires[0] ? tires : texfilter::StatusText());
    } else if (g_panel.tab == kTabKeybinds) {
        snprintf(readout, sizeof readout, "CLICK A KEY, THEN PRESS THE NEW ONE   \xB7   ESC CANCELS   \xB7   SAVE TO KEEP THEM");
    } else if (g_panel.tab == kTabOther) {
        const char* status = GameOverlayStatusText();
        snprintf(readout, sizeof readout, "%s", status[0] ? status : "THE GAME'S MENU (ESC) SHOWS A HIDDEN HUD, THE MOUSE A HIDDEN CURSOR");
    } else if (!cfg.enabled) {
        snprintf(readout, sizeof readout, "FIRST PERSON MOD IS OFF%s",
                 g_panel.tab == kTabReplay ? " - REPLAYS USE THE GAME'S OWN CAMERA" : "");
    } else if (g_panel.tab == kTabFirstPerson) {
        char view[96];
        if (cfg.customCamera && !t.bikeTracked)
            snprintf(view, sizeof view, "%s - RIDE A FEW METRES", bike::StatusText());
        else
            snprintf(view, sizeof view, "%s   PITCH %+.0f\xB0   LEAN %.0f\xB0",
                     t.customActive ? "CUSTOM CAM" : (t.bikeTracked ? "BIKE FOUND" : "GAME CAM"),
                     t.pitchExcessDeg, std::fabs(t.rawRollDeg));
        snprintf(readout, sizeof readout, "%s   \xB7   %s   LOOK %+.0f\xB0   %s   \xB7   LAG %.1f\xB0   %.1f G   NECK %+.0f MM",
                 view, t.airborne > 0.5 ? "AIR" : "GROUND", t.lookYawDeg,
                 cfg.controllerLook ? gamepad::StatusText() : "LOOK OFF", t.lagDeg, 1.0 + t.verticalAccelG,
                 t.neckOffset * 1000.0);
    } else {
        snprintf(readout, sizeof readout, "%s", replay::StatusText());
    }
    Text(kFontSmall, x, y, readout, kSection, S(1));
    y += S(36);

    // ---- Buttons (right), hint / status (left) ----
    const auto Button = [&](int buttonItem, float bx, float bw, const char* label, bool primary, const char* help) {
        const bool hot = in.Inside(bx, y, bw, buttonH);
        OfferHelp(help, y, y + buttonH, bx, bw, hot, g_panel.selected == buttonItem);
        if (hot && in.pressed) {
            g_panel.pressedItem = buttonItem;
            g_panel.selected = buttonItem;
        }
        const bool clicked = (hot && in.released && g_panel.pressedItem == buttonItem) ||
                             (g_panel.selected == buttonItem && in.activate);
        const bool focused = hot || g_panel.selected == buttonItem;
        const float r = S(6);
        uint32_t textColor;
        if (primary || (hot && in.down)) {
            RoundedRect(bx - 1, y - 1, bw + 2, buttonH + 2, r + 1, Rgba(96, 156, 244, 1.0f));
            RoundedRect(bx, y, bw, buttonH, r, focused ? kBlueHot : kBlue);
            textColor = White(1.0f);
        } else {
            RoundedRect(bx, y, bw, buttonH, r, focused ? kBlueHot : kBorder);
            RoundedRect(bx + 1, y + 1, bw - 2, buttonH - 2, r - 1, focused ? Rgba(20, 36, 66, 1.0f) : kPanelSolid);
            textColor = White(focused ? 1.0f : 0.88f);
        }
        const float labelW = TextWidth(kFontSmall, label, S(2));
        Text(kFontSmall, std::round(bx + (bw - labelW) * 0.5f), y + std::round((buttonH - LineHeight(kFontSmall)) * 0.5f),
             label, textColor, S(2));
        return clicked;
    };

    const float gap = S(10), bw = S(180);
    const float buttonsX = x + innerW - 3 * bw - 2 * gap;
    if (Button(saveItem, buttonsX, bw, g_panel.dirty ? "SAVE" : "SAVED", g_panel.dirty,
               "Writes your settings and keybinds to mxcam.ini so they load next time.")) {
        if (SaveConfig()) {
            g_panel.dirty = false;
            SetStatus("SAVED TO MXCAM.INI", now);
        } else {
            SetStatus("COULD NOT WRITE MXCAM.INI", now);
        }
    }
    if (Button(defaultsItem, buttonsX + bw + gap, bw, "DEFAULTS", false,
               "Puts every first person, replay, graphics and other setting of this preset back to its "
               "default (not the keybinds). Press SAVE to keep it.")) {
        ResetTunables();
        camera::Reset();
        g_panel.dirty = true;
        SetStatus("DEFAULTS RESTORED - NOT SAVED YET", now);
    }
    if (Button(closeItem, buttonsX + 2 * (bw + gap), bw, "CLOSE", false,
               "Closes the menu. Unsaved changes stay until you quit the game."))
        g_panel.closeRequest = true;

    // ---- Presets (left): PRESET 1 | 2 | 3 ----
    float hintX = x;
    {
        const char* label = "PRESET";
        const float labelW = TextWidth(kFontSmall, label, S(2));
        const float segH = S(32), segW = S(46), segY = std::round(y + (buttonH - segH) * 0.5f);
        const float sx0 = x + labelW + S(14), total = kPresetCount * segW;
        const bool focused = g_panel.selected == presetItem;
        const bool areaHot = in.Inside(x, y, sx0 + total - x, buttonH);
        OfferHelp("Three presets, each with its own copy of every setting (not the keybinds). Click one to "
                  "switch to it: your changes stay with the preset you made them in. SAVE writes all three "
                  "to mxcam.ini, and the game starts with the one in use.",
                  y, y + buttonH, x, sx0 + total - x, areaHot, focused);
        if (focused)
            Marker(px + S(10), segY, segH);
        Text(kFontSmall, x, y + std::round((buttonH - LineHeight(kFontSmall)) * 0.5f), label, focused ? kLabel : kDim, S(2));
        const int current = ActivePreset();
        int wanted = current;
        RoundedRect(sx0 - 1, segY - 1, total + 2, segH + 2, segH * 0.5f + 1, kBorder);
        RoundedRect(sx0, segY, total, segH, segH * 0.5f, kPanelSolid);
        for (int p = 0; p < kPresetCount; ++p) {
            const float sx = sx0 + p * segW;
            const bool hot = in.Inside(sx, segY, segW, segH);
            if (hot && in.pressed) {
                wanted = p;
                g_panel.selected = presetItem;
            }
            if (p == current)
                RoundedRect(sx, segY, segW, segH, segH * 0.5f, hot ? kBlueHot : kBlue);
            char number[4];
            snprintf(number, sizeof number, "%d", p + 1);
            const float numberW = TextWidth(kFontSmall, number, S(2));
            Text(kFontSmall, std::round(sx + (segW - numberW) * 0.5f), segY + std::round((segH - LineHeight(kFontSmall)) * 0.5f),
                 number, p == current ? White(1.0f) : (hot ? kLabel : kDim), S(2));
        }
        if (focused && in.step)
            wanted = Clamp(current + (in.step > 0 ? 1 : -1), 0, kPresetCount - 1);
        if (focused && in.activate)
            wanted = (current + 1) % kPresetCount;
        if (wanted != current) {
            SwitchPreset(wanted);
            camera::Reset();
            g_panel.dirty = true;
            char text[64];
            snprintf(text, sizeof text, "PRESET %d IN USE - SAVE TO KEEP IT", wanted + 1);
            SetStatus(text, now);
        }
        hintX = sx0 + total + S(24);
    }

    {
        const float hintY = y + std::round((buttonH - LineHeight(kFontSmall)) * 0.5f);
        const float room = buttonsX - S(12) - hintX;
        // Whatever doesn't fit beside the presets goes on the readout line above instead.
        const auto Hint = [&](const char* text, uint32_t color) {
            if (TextWidth(kFontSmall, text, S(1)) <= room)
                Text(kFontSmall, hintX, hintY, text, color, S(1));
            else
                Text(kFontSmall, x + innerW - TextWidth(kFontSmall, text, S(1)), y - S(36), text, color, S(1));
        };
        if (g_panel.listening >= 0) {
            char label[64], hint[160];
            Upper(kRows[g_panel.listening].label, label, sizeof label);
            if (kRows[g_panel.listening].kind == RowKind::Pad)
                snprintf(hint, sizeof hint, "PRESS A BUTTON OR PUSH A STICK FOR %s   \xB7   BACKSPACE CLEARS   \xB7   ESC CANCELS", label);
            else
                snprintf(hint, sizeof hint, "PRESS THE NEW KEY FOR %s   \xB7   ESC CANCELS", label);
            Hint(hint, White(1.0f));
        } else if (now < g_panel.statusUntil) {
            Hint(g_panel.status, White(1.0f));
        } else {
            char hint[128];
            snprintf(hint, sizeof hint, "%s TO CLOSE   \xB7   ARROW KEYS ADJUST", KeyName(cfg.menuKey));
            Hint(hint, kDim);
        }
    }
    y += buttonH + S(14);

    // ---- Credit, bottom right ----
    {
        const char* author = "By: Voaks";
        Text(kFontSmall, x + innerW - TextWidth(kFontSmall, author, S(1)), y, author, kDim, S(1));
    }

    if (in.released || !in.down) {
        g_panel.dragging = -1;
        if (in.released)
            g_panel.pressedItem = -1;
    }

    // ---- Help box: beside the panel, level with its row; on a screen too narrow for that, just
    // under (or over) the row in its own column ----
    const HelpSpot& help = g_panel.helpForMouse ? hoveredHelp : selectedHelp;
    if (help.text) {
        const float padX = S(16), padY = S(12), lineH = LineHeight(kFontBody), margin = S(8);
        const bool beside = px + panelW + S(14) + S(340) <= screenWidth - margin;
        const float boxW = beside ? S(340) : std::fmax(help.width, S(300));
        char lines[kMaxWrapLines][kWrapLineSize];
        const int count = WrapText(kFontBody, help.text, boxW - 2 * padX, lines);
        const float boxH = 2 * padY + count * lineH;
        float bx, by;
        if (beside) {
            bx = px + panelW + S(14);
            by = std::fmax(S(16), std::fmin(help.top, screenHeight - boxH - S(16)));
        } else {
            bx = std::fmax(margin, std::fmin(help.left, screenWidth - boxW - margin));
            by = help.bottom + S(6);
            if (by + boxH > screenHeight - margin)
                by = std::fmax(margin, help.top - boxH - S(6));
        }
        PanelFill(bx, by, boxW, boxH, S(8));
        for (int i = 0; i < count; ++i)
            Text(kFontBody, bx + padX, by + padY + i * lineH, lines[i], kLabel, 0);
    }

    // Our own cursor (the system one is hidden over the game window while the panel is open).
    if (in.inside) {
        const size_t cursorBegin = g_batch.size();
        Disc(in.mx, in.my, S(6) + 1.5f, Rgba(0, 0, 0, 0.55f));
        Disc(in.mx, in.my, S(5), White(1.0f));
        g_keepRanges.push_back({ cursorBegin, g_batch.size() });
    }
}

// The fade (Config::fadeMenu): toward nearly see-through while a GRAPHICS slider is being dragged
// or stepped, back when it's let go; quick out, a little slower back in.
void UpdateFade(double now)
{
    const Config& cfg = GetConfig();
    const double dt = g_panel.lastFrame > 0 ? Clamp(now - g_panel.lastFrame, 0.0, 0.1) : 0.0;
    g_panel.lastFrame = now;
    // For automated tests: as if the GRAPHICS tab's first slider were being stepped.
    static const bool testFade = GetEnvironmentVariableA("MXCAM_TEST_MENU_FADE", nullptr, 0) > 0;
    if (testFade) {
        for (int i = 0; i < kRowCount; ++i)
            if (kRows[i].tab == kTabGraphics && kRows[i].kind == RowKind::Slider) {
                g_panel.adjustRow = i;
                g_panel.adjustedAt = now;
                break;
            }
    }
    const int row = g_panel.adjustRow;
    const bool graphicsSlider = row >= 0 && row < kRowCount && kRows[row].tab == kTabGraphics &&
                                kRows[row].kind == RowKind::Slider && g_panel.tab == kTabGraphics;
    const bool adjusting = cfg.fadeMenu && graphicsSlider &&
                           (g_panel.dragging == row || (g_panel.adjustedAt >= 0 && now - g_panel.adjustedAt < kAdjustHold));
    const float target = adjusting ? kFadedOpacity : 1.0f;
    const double tau = target < g_panel.opacity ? 0.08 : 0.2;
    g_panel.opacity = target + (g_panel.opacity - target) * static_cast<float>(std::exp(-dt / tau));
    if (std::fabs(g_panel.opacity - target) < 0.002f)
        g_panel.opacity = target;
    if (!adjusting && g_panel.opacity >= 1.0f && g_panel.dragging != row)
        g_panel.adjustRow = -1;
}

// Everything drawn this frame but the kept parts, at the fade's opacity.
void ApplyFade()
{
    if (g_panel.opacity >= 1.0f)
        return;
    size_t next = 0;  // the kept ranges are in batch order
    for (size_t v = 0; v < g_batch.size(); ++v) {
        while (next < g_keepRanges.size() && v >= g_keepRanges[next].end)
            ++next;
        if (next < g_keepRanges.size() && v >= g_keepRanges[next].begin)
            continue;
        uint32_t& color = g_batch[v].color;
        const uint32_t alpha = static_cast<uint32_t>((color >> 24) * g_panel.opacity + 0.5f);
        color = (color & 0x00FFFFFFu) | (alpha << 24);
    }
}

void DrawBatch(int width, int height)
{
    if (g_batch.empty())
        return;
    if (g_gl.BindFramebuffer)
        g_gl.BindFramebuffer(kDrawFramebuffer, 0);
    g_gl.Viewport(0, 0, width, height);
    g_gl.Disable(GL_DEPTH_TEST);
    g_gl.Disable(GL_CULL_FACE);
    g_gl.Disable(GL_SCISSOR_TEST);
    g_gl.Disable(GL_STENCIL_TEST);
    if (g_gl.BindFramebuffer)
        g_gl.Disable(kFramebufferSrgb);
    g_gl.Enable(GL_BLEND);
    g_gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    g_gl.BlendEquation(kFuncAdd);
    g_gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    g_gl.UseProgram(g_gpu.program);
    g_gl.Uniform2f(g_gpu.screenUniform, static_cast<GLfloat>(width), static_cast<GLfloat>(height));
    g_gl.Uniform1i(g_gpu.atlasUniform, 0);
    g_gl.BindTexture(GL_TEXTURE_2D, g_gpu.texture);  // unit 0 (made active by SaveState)
    if (g_gl.BindSampler)
        g_gl.BindSampler(0, 0);

    g_gl.BindVertexArray(g_gpu.vao);
    g_gl.BindBuffer(kArrayBuffer, g_gpu.vbo);
    g_gl.BufferData(kArrayBuffer, static_cast<ptrdiff_t>(g_batch.size() * sizeof(Vertex)), g_batch.data(), kStreamDraw);
    g_gl.DrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(g_batch.size()));
}

// ===========================================================================
// The startup hint: every time the game starts, which key opens this menu (players kept asking).
// Shown kHintHold seconds from the first frame, then faded out over kHintFade; gone for good once
// the menu is opened. The key is the menu key as it is now, so a rebind shows on it at once.
// ===========================================================================
constexpr double kHintHold = 5.0;
constexpr double kHintFade = 1.0;
double g_hintStart = -1;
bool   g_hintDone  = false;

uint32_t Faded(uint32_t color, float a)
{
    const uint32_t alpha = static_cast<uint32_t>((color >> 24) * Clamp(a, 0.0f, 1.0f) + 0.5f);
    return (color & 0x00FFFFFFu) | (alpha << 24);
}

void BuildStartupHint(int width, int height, double now)
{
    const float s = g_atlasScale;
    const auto S = [s](float v) { return std::round(v * s); };
    const double t = now - g_hintStart;
    const float a = t < kHintHold ? 1.0f : static_cast<float>(1.0 - (t - kHintHold) / kHintFade);
    const char* lead = "MXS Remastered:";
    char rest[96];
    snprintf(rest, sizeof rest, "Press \"%s\" to open menu", KeyName(GetConfig().menuKey));
    // In the middle of the screen, in the menu's title font, the corners only a little rounded.
    const float gap = S(14), padX = S(40), padY = S(24);
    const float leadW = TextWidth(kFontTitle, lead, S(1)), restW = TextWidth(kFontTitle, rest, 0);
    const float w = padX * 2 + leadW + gap + restW, h = padY * 2 + LineHeight(kFontTitle);
    const float x = std::round((width - w) * 0.5f), y = std::round((height - h) * 0.5f), r = S(8);
    RoundedRect(x - S(2), y - S(2), w + S(4), h + S(4), r + S(2), Faded(kBorder, a));
    RoundedRect(x, y, w, h, r, Faded(kPanelColor, a));
    Text(kFontTitle, x + padX, y + padY, lead, Faded(kBlueHot, a), S(1));
    Text(kFontTitle, x + padX + leadW + gap, y + padY, rest, Faded(White(1.0f), a), 0);
}

// Draws the startup hint over this frame (the menu closed), while it lasts.
void StartupHintFrame(HWND window, double now)
{
    if (g_hintDone)
        return;
    if (g_hintStart < 0)
        g_hintStart = now;
    if (now - g_hintStart > kHintHold + kHintFade) {
        g_hintDone = true;
        return;
    }
    RECT client;
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
        return;
    const int width = client.right, height = client.bottom;
    const float scale = std::round(Clamp(std::fmin(height / 1080.0f, width / 1320.0f), 0.6f, 2.0f) * 20.0f) / 20.0f;  // (as the menu's)
    SavedState saved;
    SaveState(saved);
    if (EnsureResources(scale)) {
        g_batch.clear();
        g_keepRanges.clear();
        BuildStartupHint(width, height, now);
        DrawBatch(width, height);
    }
    RestoreState(saved);
}

}  // namespace

bool OverlayListeningForKey()
{
    // And a moment after: the key that answered may still be held.
    return g_panel.listening >= 0 || (g_panel.listenEnded >= 0 && Seconds() - g_panel.listenEnded < 0.5);
}

bool OverlayPanelOpen() { return g_panel.open; }

bool HideGameOverlayNow(bool cursor)
{
    const Config& cfg = GetConfig();
    if (!(cursor ? cfg.hideCursor : cfg.hideHud) || !SceneOnScreen() || GameMenuOverScene())
        return false;
    // The HUD: all the while you ride or watch (this panel open too: turned on, it goes at once).
    if (!cursor)
        return true;
    if (g_menuOpen.load(std::memory_order_relaxed))
        return false;
    return GetTickCount64() - g_lastMouseActivity.load(std::memory_order_relaxed) > kRevealMs;
}

int TakeGameEscapes()
{
    return g_gameEscapes.exchange(0);
}

bool GameCursorPos(float& x, float& y)
{
    const HWND window = g_gameWindow.load(std::memory_order_relaxed);
    POINT p;
    if (!window || !GetCursorPos(&p) || !ScreenToClient(window, &p))
        return false;
    x = static_cast<float>(p.x);
    y = static_cast<float>(p.y);
    return true;
}

void OverlayFrame(HDC dc)
{
    if (!g_glLoaded) {
        g_glLoaded = true;
        g_glAvailable = LoadGL();
        if (!g_glAvailable)
            Log("overlay: required OpenGL functions are missing; settings panel disabled");
    }

    const double now = Seconds();
    const Config& cfg = GetConfig();
    const bool focus = GameHasFocus();
    const HWND window = WindowFromDC(dc);
    g_gameWindow = window;
    NoteMouseActivity();
    UpdateFullscreenWindow(window, cfg.fullscreenWindow);
    UpdateSoundPacks(now);
    // The game's cursor while riding (Config::hideCursor): if it's the system's, the window
    // procedure keeps it hidden; it's put away here the moment hiding starts.
    if (cfg.hideCursor && window && g_glAvailable) {
        SubclassWindow(window);
        static bool wasHidden = false;
        const bool hidden = HideGameOverlayNow(true);
        float mx = 0, my = 0;
        RECT area;
        if (hidden && !wasHidden && focus && GameCursorPos(mx, my) && GetClientRect(window, &area) && mx >= 0 &&
            my >= 0 && mx < area.right && my < area.bottom)
            SetCursor(nullptr);
        wasHidden = hidden;
    }

    // Polled every frame, even while closed, so a key held during the toggle doesn't re-fire.
    const bool menuKey = PollKey(g_keys.menu, cfg.menuKey, false, now);
    const bool escape  = PollKey(g_keys.escape, VK_ESCAPE, false, now);
    // The game's menu (Config::hideHud): Esc presses are counted by the window procedure; if the
    // game's window never gets key messages (input read some other way), from the key itself.
    if (cfg.hideHud && window && g_glAvailable)
        SubclassWindow(window);
    if (escape && focus && !g_panel.open && !g_keyMessages.load(std::memory_order_relaxed))
        ++g_gameEscapes;

    // For automated tests: open the panel without a key press.
    static bool testOpenChecked = false;
    bool testOpen = false;
    if (!testOpenChecked) {
        testOpenChecked = true;
        testOpen = GetEnvironmentVariableA("MXCAM_TEST_OPEN_MENU", nullptr, 0) > 0;
        char tab[8] = "";
        if (GetEnvironmentVariableA("MXCAM_TEST_MENU_TAB", tab, sizeof tab) > 0)
            g_panel.tab = Clamp(atoi(tab), 0, kTabCount - 1);
    }

    if (!g_panel.open) {
        if (!window || !g_glAvailable)
            return;
        if (!((menuKey && focus) || testOpen)) {
            StartupHintFrame(window, now);  // (which key opens the menu, for a few seconds after the start)
            return;
        }
        g_hintDone = true;  // (they've found it)
        OpenPanel(window);
    } else if (g_panel.closeRequest || (focus && g_panel.listening < 0 && (menuKey || escape))) {
        ClosePanel();
        return;
    }
    if (!window)
        return;

    RECT client;
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
        return;
    const int width = client.right, height = client.bottom;
    Input in = ReadInput(window, focus, width, height, now);
    if (g_panel.listening >= 0) {
        // The next key press is the answer, not navigation.
        if (focus)
            ListenForKey(now);
        else
            g_panel.listening = -1;
        in.nav = in.step = 0;
        in.activate = false;
    }

    // UI scale follows the window height (1080p = 1.0), held down on narrow windows so the panel
    // still fits across, in 5% steps so the atlas isn't rebuilt for every pixel of a resize.
    const float scale = std::round(Clamp(std::fmin(height / 1080.0f, width / 1320.0f), 0.6f, 2.0f) * 20.0f) / 20.0f;

    SavedState saved;
    SaveState(saved);
    if (EnsureResources(scale)) {
        g_batch.clear();
        g_keepRanges.clear();
        UpdateFade(now);
        BuildPanel(width, height, in, now);
        ApplyFade();
        DrawBatch(width, height);
    }
    RestoreState(saved);
}

}  // namespace mxcam
