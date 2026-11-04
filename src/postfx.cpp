// postfx.cpp
//   The shader pipeline (see postfx.h for where its information comes from).
//
//   Per frame, once the 3D scene is complete:
//     capture     the window's colour and depth are copied into our textures (a blit, which also
//                 resolves multisampling, or a texture copy if the blit is refused)
//     lighting    half resolution: ambient occlusion and screen-space sun shadows, by marching
//                 from each pixel toward the sun through the depth buffer
//     composite   full resolution, linear colour: dirt detail on the ground (only where the depth
//                 still matches the snapshot taken when the terrain pass ended), AO and shadows,
//                 and the time of day: grading, haze, and a sunset / night sky with stars and moon
//     bloom       six-level downsample / upsample chain of what's bright
//     sun rays    radial blur of the bright sky around the sun
//     DOF         auto-focus (eased on the GPU), half resolution bokeh gather, merged by depth
//     motion blur camera motion: every pixel reprojected into last frame's view through depth
//     grade       bloom and rays added, exposure, saturation, contrast, vignette -> display
//     final       FXAA and contrast-adaptive sharpening, written back into the window
//
//   Like the settings panel: every GL call goes straight to the driver, and every piece of GL
//   state touched is saved and restored around the work.

#include <windows.h>
#include <GL/gl.h>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "bike.h"
#include "config.h"
#include "hooks.h"
#include "log.h"
#include "mathlib.h"
#include "postfx.h"
#include "postfx_shaders.h"
#include "timeofday.h"

namespace mxcam {
namespace postfx {
namespace {

// ===========================================================================
// OpenGL constants past 1.1 and the driver's entry points
// ===========================================================================
constexpr GLenum kFramebuffer               = 0x8D40;
constexpr GLenum kReadFramebuffer           = 0x8CA8;
constexpr GLenum kDrawFramebuffer           = 0x8CA9;
constexpr GLenum kReadFramebufferBinding    = 0x8CAA;
constexpr GLenum kDrawFramebufferBinding    = 0x8CA6;
constexpr GLenum kColorAttachment0          = 0x8CE0;
constexpr GLenum kDepthAttachment           = 0x8D00;
constexpr GLenum kDepthStencilAttachment    = 0x821A;
constexpr GLenum kFramebufferComplete       = 0x8CD5;
constexpr GLenum kRgba16f                   = 0x881A;
constexpr GLenum kRg8                       = 0x822B;
constexpr GLenum kRg                        = 0x8227;
constexpr GLenum kR32f                      = 0x822E;
constexpr GLenum kRed                       = 0x1903;
constexpr GLenum kHalfFloat                 = 0x140B;
constexpr GLenum kDepth24Stencil8           = 0x88F0;
constexpr GLenum kDepthStencil              = 0x84F9;
constexpr GLenum kUnsignedInt248            = 0x84FA;
constexpr GLenum kDepthComponent24          = 0x81A6;
constexpr GLenum kTextureCompareMode        = 0x884C;
constexpr GLenum kClampToEdge               = 0x812F;
constexpr GLenum kArrayBuffer               = 0x8892;
constexpr GLenum kArrayBufferBinding        = 0x8894;
constexpr GLenum kStaticDraw                = 0x88E4;
constexpr GLenum kVertexShader              = 0x8B31;
constexpr GLenum kFragmentShader            = 0x8B30;
constexpr GLenum kCompileStatus             = 0x8B81;
constexpr GLenum kLinkStatus                = 0x8B82;
constexpr GLenum kInfoLogLength             = 0x8B84;
constexpr GLenum kCurrentProgram            = 0x8B8D;
constexpr GLenum kTexture0                  = 0x84C0;
constexpr GLenum kActiveTexture             = 0x84E0;
constexpr GLenum kVertexArrayBinding        = 0x85B5;
constexpr GLenum kSamplerBinding            = 0x8919;
constexpr GLenum kBlendDstRgb               = 0x80C8;
constexpr GLenum kBlendSrcRgb               = 0x80C9;
constexpr GLenum kBlendDstAlpha             = 0x80CA;
constexpr GLenum kBlendSrcAlpha             = 0x80CB;
constexpr GLenum kBlendEquationRgb          = 0x8009;
constexpr GLenum kBlendEquationAlpha        = 0x883D;
constexpr GLenum kFuncAdd                   = 0x8006;
constexpr GLenum kFuncMax                   = 0x8008;
constexpr GLenum kR8                        = 0x8229;
constexpr GLenum kFramebufferSrgb           = 0x8DB9;
constexpr GLenum kSampleAlphaToCoverage     = 0x809E;
constexpr GLenum kRasterizerDiscard         = 0x8C89;
constexpr GLenum kPixelUnpackBuffer         = 0x88EC;
constexpr GLenum kPixelUnpackBufferBinding  = 0x88EF;
constexpr GLenum kTextureMaxAnisotropy      = 0x84FE;
constexpr GLenum kAlphaTest                 = 0x0BC0;
constexpr GLenum kTextureCompareFunc        = 0x884D;
constexpr GLenum kCompareRefToTexture       = 0x884E;

struct GLApi {
    decltype(&::glGetIntegerv)         GetIntegerv;
    decltype(&::glGetBooleanv)         GetBooleanv;
    decltype(&::glGetError)            GetError;
    decltype(&::glIsEnabled)           IsEnabled;
    decltype(&::glEnable)              Enable;
    decltype(&::glDisable)             Disable;
    decltype(&::glViewport)            Viewport;
    decltype(&::glColorMask)           ColorMask;
    decltype(&::glDepthMask)           DepthMask;
    decltype(&::glDepthFunc)           DepthFunc;
    decltype(&::glDepthRange)          DepthRange;
    decltype(&::glClear)               Clear;
    decltype(&::glClearDepth)          ClearDepth;
    decltype(&::glGetFloatv)           GetFloatv;
    decltype(&::glGenTextures)         GenTextures;
    decltype(&::glDeleteTextures)      DeleteTextures;
    decltype(&::glBindTexture)         BindTexture;
    decltype(&::glTexParameteri)       TexParameteri;
    decltype(&::glTexParameterf)       TexParameterf;
    decltype(&::glTexImage2D)          TexImage2D;
    decltype(&::glPixelStorei)         PixelStorei;
    decltype(&::glDrawArrays)          DrawArrays;
    decltype(&::glReadBuffer)          ReadBuffer;
    decltype(&::glDrawBuffer)          DrawBuffer;
    decltype(&::glCopyTexSubImage2D)   CopyTexSubImage2D;
    decltype(&::wglGetCurrentContext)  GetCurrentContext;
    decltype(&::wglGetCurrentDC)       GetCurrentDC;
    GLuint (APIENTRY* CreateShader)(GLenum);
    void   (APIENTRY* ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
    void   (APIENTRY* CompileShader)(GLuint);
    void   (APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*);
    void   (APIENTRY* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*);
    void   (APIENTRY* DeleteShader)(GLuint);
    GLuint (APIENTRY* CreateProgram)();
    void   (APIENTRY* DeleteProgram)(GLuint);
    void   (APIENTRY* AttachShader)(GLuint, GLuint);
    void   (APIENTRY* BindAttribLocation)(GLuint, GLuint, const char*);
    void   (APIENTRY* LinkProgram)(GLuint);
    void   (APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*);
    void   (APIENTRY* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, char*);
    void   (APIENTRY* UseProgram)(GLuint);
    GLint  (APIENTRY* GetUniformLocation)(GLuint, const char*);
    void   (APIENTRY* Uniform1i)(GLint, GLint);
    void   (APIENTRY* Uniform1f)(GLint, GLfloat);
    void   (APIENTRY* Uniform2f)(GLint, GLfloat, GLfloat);
    void   (APIENTRY* Uniform3f)(GLint, GLfloat, GLfloat, GLfloat);
    void   (APIENTRY* Uniform1fv)(GLint, GLsizei, const GLfloat*);
    void   (APIENTRY* UniformMatrix3fv)(GLint, GLsizei, GLboolean, const GLfloat*);
    void   (APIENTRY* UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
    void   (APIENTRY* GenBuffers)(GLsizei, GLuint*);
    void   (APIENTRY* BindBuffer)(GLenum, GLuint);
    void   (APIENTRY* BufferData)(GLenum, ptrdiff_t, const void*, GLenum);
    void   (APIENTRY* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    void   (APIENTRY* EnableVertexAttribArray)(GLuint);
    void   (APIENTRY* ActiveTexture)(GLenum);
    void   (APIENTRY* BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
    void   (APIENTRY* BlendEquationSeparate)(GLenum, GLenum);
    void   (APIENTRY* GenVertexArrays)(GLsizei, GLuint*);
    void   (APIENTRY* BindVertexArray)(GLuint);
    void   (APIENTRY* GenFramebuffers)(GLsizei, GLuint*);
    void   (APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*);
    void   (APIENTRY* BindFramebuffer)(GLenum, GLuint);
    void   (APIENTRY* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (APIENTRY* CheckFramebufferStatus)(GLenum);
    void   (APIENTRY* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void   (APIENTRY* GenerateMipmap)(GLenum);
    void   (APIENTRY* BindSampler)(GLuint, GLuint);  // optional (GL 3.3)
    decltype(&::glScissor)             Scissor;
};

GLApi g_gl{};
int   g_glState = 0;  // 0 = not loaded yet, 1 = ready, -1 = missing functions

template <class T>
bool LoadFunction(T& slot, const char* name)
{
    slot = reinterpret_cast<T>(GetRealGLFunction(name));
    if (!slot)
        Log("postfx: %s is missing", name);
    return slot != nullptr;
}

bool LoadGL()
{
    if (g_glState != 0)
        return g_glState > 0;
    bool ok = true;
#define MXCAM_LOAD(field, name) ok = LoadFunction(g_gl.field, name) && ok
    MXCAM_LOAD(GetIntegerv, "glGetIntegerv");
    MXCAM_LOAD(GetBooleanv, "glGetBooleanv");
    MXCAM_LOAD(GetError, "glGetError");
    MXCAM_LOAD(IsEnabled, "glIsEnabled");
    MXCAM_LOAD(Enable, "glEnable");
    MXCAM_LOAD(Disable, "glDisable");
    MXCAM_LOAD(Viewport, "glViewport");
    MXCAM_LOAD(ColorMask, "glColorMask");
    MXCAM_LOAD(DepthMask, "glDepthMask");
    MXCAM_LOAD(DepthFunc, "glDepthFunc");
    MXCAM_LOAD(DepthRange, "glDepthRange");
    MXCAM_LOAD(Clear, "glClear");
    MXCAM_LOAD(ClearDepth, "glClearDepth");
    MXCAM_LOAD(GetFloatv, "glGetFloatv");
    MXCAM_LOAD(GenTextures, "glGenTextures");
    MXCAM_LOAD(DeleteTextures, "glDeleteTextures");
    MXCAM_LOAD(BindTexture, "glBindTexture");
    MXCAM_LOAD(TexParameteri, "glTexParameteri");
    MXCAM_LOAD(TexParameterf, "glTexParameterf");
    MXCAM_LOAD(TexImage2D, "glTexImage2D");
    MXCAM_LOAD(PixelStorei, "glPixelStorei");
    MXCAM_LOAD(DrawArrays, "glDrawArrays");
    MXCAM_LOAD(ReadBuffer, "glReadBuffer");
    MXCAM_LOAD(DrawBuffer, "glDrawBuffer");
    MXCAM_LOAD(CopyTexSubImage2D, "glCopyTexSubImage2D");
    MXCAM_LOAD(GetCurrentContext, "wglGetCurrentContext");
    MXCAM_LOAD(GetCurrentDC, "wglGetCurrentDC");
    MXCAM_LOAD(CreateShader, "glCreateShader");
    MXCAM_LOAD(ShaderSource, "glShaderSource");
    MXCAM_LOAD(CompileShader, "glCompileShader");
    MXCAM_LOAD(GetShaderiv, "glGetShaderiv");
    MXCAM_LOAD(GetShaderInfoLog, "glGetShaderInfoLog");
    MXCAM_LOAD(DeleteShader, "glDeleteShader");
    MXCAM_LOAD(CreateProgram, "glCreateProgram");
    MXCAM_LOAD(DeleteProgram, "glDeleteProgram");
    MXCAM_LOAD(AttachShader, "glAttachShader");
    MXCAM_LOAD(BindAttribLocation, "glBindAttribLocation");
    MXCAM_LOAD(LinkProgram, "glLinkProgram");
    MXCAM_LOAD(GetProgramiv, "glGetProgramiv");
    MXCAM_LOAD(GetProgramInfoLog, "glGetProgramInfoLog");
    MXCAM_LOAD(UseProgram, "glUseProgram");
    MXCAM_LOAD(GetUniformLocation, "glGetUniformLocation");
    MXCAM_LOAD(Uniform1i, "glUniform1i");
    MXCAM_LOAD(Uniform1f, "glUniform1f");
    MXCAM_LOAD(Uniform2f, "glUniform2f");
    MXCAM_LOAD(Uniform3f, "glUniform3f");
    MXCAM_LOAD(Uniform1fv, "glUniform1fv");
    MXCAM_LOAD(UniformMatrix3fv, "glUniformMatrix3fv");
    MXCAM_LOAD(UniformMatrix4fv, "glUniformMatrix4fv");
    MXCAM_LOAD(GenBuffers, "glGenBuffers");
    MXCAM_LOAD(BindBuffer, "glBindBuffer");
    MXCAM_LOAD(BufferData, "glBufferData");
    MXCAM_LOAD(VertexAttribPointer, "glVertexAttribPointer");
    MXCAM_LOAD(EnableVertexAttribArray, "glEnableVertexAttribArray");
    MXCAM_LOAD(ActiveTexture, "glActiveTexture");
    MXCAM_LOAD(BlendFuncSeparate, "glBlendFuncSeparate");
    MXCAM_LOAD(BlendEquationSeparate, "glBlendEquationSeparate");
    MXCAM_LOAD(GenVertexArrays, "glGenVertexArrays");
    MXCAM_LOAD(BindVertexArray, "glBindVertexArray");
    MXCAM_LOAD(GenFramebuffers, "glGenFramebuffers");
    MXCAM_LOAD(DeleteFramebuffers, "glDeleteFramebuffers");
    MXCAM_LOAD(BindFramebuffer, "glBindFramebuffer");
    MXCAM_LOAD(FramebufferTexture2D, "glFramebufferTexture2D");
    MXCAM_LOAD(CheckFramebufferStatus, "glCheckFramebufferStatus");
    MXCAM_LOAD(BlitFramebuffer, "glBlitFramebuffer");
    MXCAM_LOAD(GenerateMipmap, "glGenerateMipmap");
    MXCAM_LOAD(Scissor, "glScissor");
#undef MXCAM_LOAD
    g_gl.BindSampler = reinterpret_cast<decltype(g_gl.BindSampler)>(GetRealGLFunction("glBindSampler"));
    g_glState = ok ? 1 : -1;
    if (!ok)
        Log("postfx: OpenGL 3.0 (framebuffer objects) is needed for the shaders; they stay off");
    return ok;
}

// Clears the driver's error flags (the probes below look at errors of their own).
void DrainErrors()
{
    for (int i = 0; i < 32 && g_gl.GetError() != GL_NO_ERROR; ++i) {
    }
}

// ===========================================================================
// Saved GL state
// ===========================================================================
constexpr int kUnits = 8;  // texture units the passes use

struct SavedState {
    GLint     program = 0, vao = 0, arrayBuffer = 0, activeTexture = 0, readFb = 0, drawFb = 0, readBuffer0 = 0;
    GLint     unpackBuffer = 0;
    GLint     textures[kUnits] = {}, samplers[kUnits] = {};
    GLint     viewport[4] = {}, scissor[4] = {};
    GLint     blendSrcRgb = 0, blendDstRgb = 0, blendSrcAlpha = 0, blendDstAlpha = 0, blendEqRgb = 0, blendEqAlpha = 0;
    GLboolean colorMask[4] = {}, depthMask = GL_TRUE;
    bool      blend = false, depthTest = false, cullFace = false, scissorTest = false, stencilTest = false;
    bool      srgb = false, alphaToCoverage = false, rasterizerDiscard = false, alphaTest = false;
};

bool g_compatibility = true;  // the context has fixed-function state (GL_ALPHA_TEST)

void Save(SavedState& s)
{
    g_gl.GetIntegerv(kCurrentProgram, &s.program);
    g_gl.GetIntegerv(kVertexArrayBinding, &s.vao);
    g_gl.GetIntegerv(kArrayBufferBinding, &s.arrayBuffer);
    g_gl.GetIntegerv(kPixelUnpackBufferBinding, &s.unpackBuffer);
    g_gl.GetIntegerv(kActiveTexture, &s.activeTexture);
    for (int unit = 0; unit < kUnits; ++unit) {
        g_gl.ActiveTexture(kTexture0 + unit);
        g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s.textures[unit]);
        if (g_gl.BindSampler)
            g_gl.GetIntegerv(kSamplerBinding, &s.samplers[unit]);
    }
    g_gl.GetIntegerv(kReadFramebufferBinding, &s.readFb);
    g_gl.GetIntegerv(kDrawFramebufferBinding, &s.drawFb);
    g_gl.BindFramebuffer(kReadFramebuffer, 0);
    g_gl.GetIntegerv(GL_READ_BUFFER, &s.readBuffer0);  // the window's own read buffer
    g_gl.GetIntegerv(GL_VIEWPORT, s.viewport);
    g_gl.GetIntegerv(GL_SCISSOR_BOX, s.scissor);
    g_gl.GetIntegerv(kBlendSrcRgb, &s.blendSrcRgb);
    g_gl.GetIntegerv(kBlendDstRgb, &s.blendDstRgb);
    g_gl.GetIntegerv(kBlendSrcAlpha, &s.blendSrcAlpha);
    g_gl.GetIntegerv(kBlendDstAlpha, &s.blendDstAlpha);
    g_gl.GetIntegerv(kBlendEquationRgb, &s.blendEqRgb);
    g_gl.GetIntegerv(kBlendEquationAlpha, &s.blendEqAlpha);
    g_gl.GetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
    g_gl.GetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
    s.blend             = g_gl.IsEnabled(GL_BLEND) != GL_FALSE;
    s.depthTest         = g_gl.IsEnabled(GL_DEPTH_TEST) != GL_FALSE;
    s.cullFace          = g_gl.IsEnabled(GL_CULL_FACE) != GL_FALSE;
    s.scissorTest       = g_gl.IsEnabled(GL_SCISSOR_TEST) != GL_FALSE;
    s.stencilTest       = g_gl.IsEnabled(GL_STENCIL_TEST) != GL_FALSE;
    s.srgb              = g_gl.IsEnabled(kFramebufferSrgb) != GL_FALSE;
    s.alphaToCoverage   = g_gl.IsEnabled(kSampleAlphaToCoverage) != GL_FALSE;
    s.rasterizerDiscard = g_gl.IsEnabled(kRasterizerDiscard) != GL_FALSE;
    s.alphaTest         = g_compatibility && g_gl.IsEnabled(kAlphaTest) != GL_FALSE;
}

void SetCapability(GLenum capability, bool enabled)
{
    if (enabled)
        g_gl.Enable(capability);
    else
        g_gl.Disable(capability);
}

// The state every pass draws with.
void PrepareState()
{
    g_gl.Disable(GL_BLEND);
    g_gl.Disable(GL_DEPTH_TEST);
    g_gl.Disable(GL_CULL_FACE);
    g_gl.Disable(GL_SCISSOR_TEST);
    g_gl.Disable(GL_STENCIL_TEST);
    g_gl.Disable(kFramebufferSrgb);
    g_gl.Disable(kSampleAlphaToCoverage);
    g_gl.Disable(kRasterizerDiscard);
    if (g_compatibility)
        g_gl.Disable(kAlphaTest);
    g_gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g_gl.DepthMask(GL_TRUE);  // depth blits honour it
    g_gl.BindBuffer(kPixelUnpackBuffer, 0);
}

void Restore(const SavedState& s)
{
    g_gl.BindVertexArray(s.vao);
    g_gl.BindBuffer(kArrayBuffer, s.arrayBuffer);
    g_gl.BindBuffer(kPixelUnpackBuffer, s.unpackBuffer);
    g_gl.UseProgram(s.program);
    for (int unit = 0; unit < kUnits; ++unit) {
        g_gl.ActiveTexture(kTexture0 + unit);
        g_gl.BindTexture(GL_TEXTURE_2D, s.textures[unit]);
        if (g_gl.BindSampler)
            g_gl.BindSampler(unit, s.samplers[unit]);
    }
    g_gl.ActiveTexture(s.activeTexture);
    g_gl.BindFramebuffer(kReadFramebuffer, 0);
    g_gl.ReadBuffer(s.readBuffer0);
    g_gl.BindFramebuffer(kReadFramebuffer, s.readFb);
    g_gl.BindFramebuffer(kDrawFramebuffer, s.drawFb);
    g_gl.Viewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    g_gl.Scissor(s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]);
    g_gl.BlendFuncSeparate(s.blendSrcRgb, s.blendDstRgb, s.blendSrcAlpha, s.blendDstAlpha);
    g_gl.BlendEquationSeparate(s.blendEqRgb, s.blendEqAlpha);
    g_gl.ColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    g_gl.DepthMask(s.depthMask);
    SetCapability(GL_BLEND, s.blend);
    SetCapability(GL_DEPTH_TEST, s.depthTest);
    SetCapability(GL_CULL_FACE, s.cullFace);
    SetCapability(GL_SCISSOR_TEST, s.scissorTest);
    SetCapability(GL_STENCIL_TEST, s.stencilTest);
    SetCapability(kFramebufferSrgb, s.srgb);
    SetCapability(kSampleAlphaToCoverage, s.alphaToCoverage);
    SetCapability(kRasterizerDiscard, s.rasterizerDiscard);
    if (g_compatibility)
        SetCapability(kAlphaTest, s.alphaTest);
}

// ===========================================================================
// GPU resources (per GL context)
// ===========================================================================
struct Program {
    GLuint id = 0;
    std::unordered_map<std::string, GLint> locations;

    GLint operator[](const char* name)
    {
        const auto found = locations.find(name);
        if (found != locations.end())
            return found->second;
        const GLint location = g_gl.GetUniformLocation(id, name);
        locations.emplace(name, location);
        return location;
    }
};

enum ProgramId { kLighting, kComposite, kBloomDown, kBloomUp, kRays, kFocus, kDof, kDofMerge, kMotion, kGrade,
                 kFinal, kStamp, kProgramCount };

struct ProgramSource {
    const char* name;
    const char* body;
};
const ProgramSource kSources[kProgramCount] = {
    { "lighting", shaders::kLighting },   { "composite", shaders::kComposite }, { "bloom down", shaders::kBloomDown },
    { "bloom up", shaders::kBloomUp },    { "sun rays", shaders::kRays },        { "focus", shaders::kFocus },
    { "dof", shaders::kDof },             { "dof merge", shaders::kDofMerge },   { "motion blur", shaders::kMotion },
    { "grade", shaders::kGrade },         { "final", shaders::kFinal },          { "tire marks", shaders::kStamp },
};

struct Target {
    GLuint tex = 0, fbo = 0;
    int    w = 0, h = 0;
};

constexpr int kBloomLevels = 6;
constexpr int kDetailSize  = 256;

struct Resources {
    HGLRC   context = nullptr;
    bool    failed  = false;
    GLuint  vao = 0, vbo = 0;
    Program programs[kProgramCount];
    GLuint  detail = 0;
    int     width = 0, height = 0;
    GLuint  captureFbo = 0, color = 0, depth = 0;  // the window's image
    GLuint  terrainFbo = 0, terrainDepth = 0;      // depth when the terrain pass ended
    GLuint  shadowFbo = 0, shadowMap = 0;          // depth seen from the sun
    Target  marks;                                 // tire marks on the ground (see TireMarks)
    bool    marksCleared = false;
    int     shadowSize = 0;
    int     colorMode = 0;  // 0 = untried, 1 = blit, 2 = copy, -1 = nothing works
    int     depthMode = 0;  // 0 = untried, 1 = blit (depth24 + stencil8), 2 = blit (depth24), 3 = copy, -1 = none
    bool    colorVerified = false, depthVerified = false;
    Target  hdr, a, b, ldr, light, dof, rays, focus[2];
    Target  bloom[kBloomLevels];
    int     focusIndex = 0;
    bool    focusValid = false;
};
Resources g_res;

// ---- Per-frame scene information (from the hooks) -----------------------------------------
struct Frame {
    bool   haveView = false;
    double raw[16] = {}, rendered[16] = {};
    bool   applied = false;
    bool   terrainSnapshot = false;
    bool   haveLight = false;
    float  lightEye[3] = {};
};
Frame  g_frame;
double g_lastRaw[16] = {};
bool   g_haveLastRaw = false;
bool   g_lastFrame3D = false;  // the last frame drew the 3D scene (for draws before this one's camera)

float  g_projCamera[16] = {}, g_projOther[16] = {};
bool   g_haveProjCamera = false, g_haveProjOther = false;

Vec3   g_gameSun{ 0.35, 0.8, 0.45 };  // world, toward the game's sun (a guess until the light is seen)
bool   g_haveGameSun = false;

struct Motion {
    bool   valid = false;
    double viewProj[16] = {};
    Vec3   eye, forward;
};
Motion g_motion;

// The sun's camera for this frame's shadow map (set up by the first object that casts).
struct ShadowFrame {
    bool   setup = false, usable = false, cleared = false, drawn = false;
    double toLight[16] = {};    // the drawn camera's eye space -> the sun's view
    float  projection[16] = {}; // the sun's orthographic projection
    double half = 25.0;         // m, half the side of the square the map covers
};
ShadowFrame g_shadow;

double g_lastApplyTime = -1;
double g_lastDt = 1.0 / 60.0;
char   g_status[128] = "";
bool   g_lastDepthOk = false, g_lastTerrainMask = false;

// Logged once each.
bool g_loggedBeforeHud = false, g_loggedEndOfFrame = false, g_loggedNoProjection = false, g_loggedNoLight = false;
bool g_loggedTerrainFbo = false, g_loggedFirstApply = false;

double Seconds()
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
}

// ---- Shaders --------------------------------------------------------------------------------
void LogInfo(const char* what, const std::string& text)
{
    Log("postfx: %s:", what);
    size_t start = 0;
    for (int lines = 0; start < text.size() && lines < 24; ++lines) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        if (end > start)
            Log("    %.*s", static_cast<int>(end - start), text.c_str() + start);
        start = end + 1;
    }
}

GLuint CompileShader(GLenum type, const char* version, const char* a, const char* b, std::string& error)
{
    const char* sources[] = { version, a, b ? b : "" };
    const GLuint shader = g_gl.CreateShader(type);
    g_gl.ShaderSource(shader, 3, sources, nullptr);
    g_gl.CompileShader(shader);
    GLint ok = 0;
    g_gl.GetShaderiv(shader, kCompileStatus, &ok);
    if (!ok) {
        GLint length = 0;
        g_gl.GetShaderiv(shader, kInfoLogLength, &length);
        error.assign(static_cast<size_t>(length > 1 ? length : 1), '\0');
        g_gl.GetShaderInfoLog(shader, length, nullptr, &error[0]);
        g_gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint BuildProgram(const char* version, const char* body, std::string& error)
{
    const GLuint vs = CompileShader(kVertexShader, version, shaders::kVertex, nullptr, error);
    if (!vs)
        return 0;
    const GLuint fs = CompileShader(kFragmentShader, version, shaders::kPrelude, body, error);
    if (!fs) {
        g_gl.DeleteShader(vs);
        return 0;
    }
    const GLuint program = g_gl.CreateProgram();
    g_gl.AttachShader(program, vs);
    g_gl.AttachShader(program, fs);
    g_gl.BindAttribLocation(program, 0, "a_pos");
    g_gl.LinkProgram(program);
    g_gl.DeleteShader(vs);  // freed with the program
    g_gl.DeleteShader(fs);
    GLint linked = 0;
    g_gl.GetProgramiv(program, kLinkStatus, &linked);
    if (!linked) {
        GLint length = 0;
        g_gl.GetProgramiv(program, kInfoLogLength, &length);
        error.assign(static_cast<size_t>(length > 1 ? length : 1), '\0');
        g_gl.GetProgramInfoLog(program, length, nullptr, &error[0]);
        g_gl.DeleteProgram(program);
        return 0;
    }
    return program;
}

bool BuildPrograms()
{
    const char* const versions[] = { "#version 150\n", "#version 130\n" };
    for (const char* version : versions) {
        bool ok = true;
        std::string error;
        int failed = -1;
        for (int i = 0; i < kProgramCount && ok; ++i) {
            g_res.programs[i] = Program{};
            g_res.programs[i].id = BuildProgram(version, kSources[i].body, error);
            if (!g_res.programs[i].id) {
                ok = false;
                failed = i;
            }
        }
        if (ok) {
            Log("postfx: shaders ready (GLSL %.3s)", version + 9);
            return true;
        }
        LogInfo((std::string("the '") + kSources[failed].name + "' shader failed with GLSL " + std::string(version + 9, 3)).c_str(), error);
        for (Program& program : g_res.programs) {
            if (program.id)
                g_gl.DeleteProgram(program.id);
            program = Program{};
        }
    }
    return false;
}

// ---- Dirt detail texture: tiling, made once on the CPU ----------------------------------------
uint32_t Hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

double Lattice(int x, int y, int period, uint32_t seed)
{
    x = ((x % period) + period) % period;
    y = ((y % period) + period) % period;
    return (Hash(static_cast<uint32_t>(x) * 73856093U ^ static_cast<uint32_t>(y) * 19349663U ^ seed) & 0xFFFF) / 65535.0;
}

// Smooth value noise that repeats every `period` lattice cells.
double ValueNoise(double u, double v, int period, uint32_t seed)
{
    const int x0 = static_cast<int>(std::floor(u)), y0 = static_cast<int>(std::floor(v));
    double fx = u - x0, fy = v - y0;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    const double a = Lattice(x0, y0, period, seed), b = Lattice(x0 + 1, y0, period, seed);
    const double c = Lattice(x0, y0 + 1, period, seed), d = Lattice(x0 + 1, y0 + 1, period, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

// RGBA: the detail's normal (x, y), its height, and an albedo variation; all centred on 0.5.
std::vector<uint8_t> MakeDetailTexture(int n)
{
    std::vector<double> height(static_cast<size_t>(n) * n), tone(height.size());
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const double u = static_cast<double>(x) / n, v = static_cast<double>(y) / n;
            double sum = 0, amplitude = 0.5, total = 0;
            for (int octave = 0; octave < 5; ++octave) {
                const int period = 8 << octave;
                sum += amplitude * ValueNoise(u * period, v * period, period, 0x9E37U * (octave + 1));
                total += amplitude;
                amplitude *= 0.55;
            }
            const double fbm = sum / total;
            const double speckle = ValueNoise(u * 128, v * 128, 128, 0xBEEFU);
            height[y * n + x] = fbm * 0.6;
            tone[y * n + x] = 0.5 + (speckle - 0.5) * 0.6 + (fbm - 0.5) * 0.35;
        }
    }
    // Pebbles and small clods.
    uint32_t seed = 12345;
    const auto random = [&seed]() { seed = Hash(seed + 0x9E3779B9U); return (seed & 0xFFFFFF) / 16777215.0; };
    for (int p = 0; p < 340; ++p) {
        const double cx = random() * n, cy = random() * n;
        const double radius = 1.0 + random() * random() * 6.0;
        const double lift = 0.4 + random() * 0.6;
        const double shade = random();
        const int reach = static_cast<int>(std::ceil(radius));
        for (int dy = -reach; dy <= reach; ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                const int px = static_cast<int>(std::floor(cx)) + dx, py = static_cast<int>(std::floor(cy)) + dy;
                const double ex = px + 0.5 - cx, ey = py + 0.5 - cy;
                const double d = std::sqrt(ex * ex + ey * ey) / radius;
                if (d >= 1.0)
                    continue;
                const size_t i = static_cast<size_t>(((py % n) + n) % n) * n + ((px % n) + n) % n;
                const double bump = std::sqrt(1.0 - d * d);
                height[i] = std::fmax(height[i], 0.22 + 0.6 * bump * lift);
                const double edge = Clamp(bump / 0.35, 0.0, 1.0);
                tone[i] = tone[i] + ((0.25 + 0.55 * shade) - tone[i]) * edge;
            }
        }
    }
    double lo = 1e9, hi = -1e9, mean = 0;
    for (double h : height) {
        lo = std::fmin(lo, h);
        hi = std::fmax(hi, h);
    }
    for (double& h : height) {
        h = (h - lo) / std::fmax(hi - lo, 1e-9);
        mean += h;
    }
    mean /= static_cast<double>(height.size());

    std::vector<uint8_t> rgba(height.size() * 4);
    const auto H = [&](int x, int y) { return height[static_cast<size_t>(((y % n) + n) % n) * n + ((x % n) + n) % n]; };
    const auto Byte = [](double v) { return static_cast<uint8_t>(Clamp(v, 0.0, 1.0) * 255.0 + 0.5); };
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const double strength = 6.0;
            const Vec3 normal = Normalize(Vec3{ -(H(x + 1, y) - H(x - 1, y)) * strength, -(H(x, y + 1) - H(x, y - 1)) * strength, 1.0 });
            uint8_t* out = &rgba[(static_cast<size_t>(y) * n + x) * 4];
            out[0] = Byte(normal.x * 0.5 + 0.5);
            out[1] = Byte(normal.y * 0.5 + 0.5);
            out[2] = Byte(H(x, y) - mean + 0.5);
            out[3] = Byte(tone[static_cast<size_t>(y) * n + x]);
        }
    }
    return rgba;
}

bool CreateDetailTexture()
{
    const std::vector<uint8_t> pixels = MakeDetailTexture(kDetailSize);
    GLint alignment = 4, rowLength = 0, skipRows = 0, skipPixels = 0;
    g_gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    g_gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    g_gl.GetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    g_gl.GetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
    g_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    g_gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    g_gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);

    g_gl.GenTextures(1, &g_res.detail);
    g_gl.BindTexture(GL_TEXTURE_2D, g_res.detail);
    g_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kDetailSize, kDetailSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    g_gl.GenerateMipmap(GL_TEXTURE_2D);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    DrainErrors();
    g_gl.TexParameterf(GL_TEXTURE_2D, kTextureMaxAnisotropy, 8.0f);  // grazing angles; ignored if unsupported
    DrainErrors();

    g_gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
    g_gl.PixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
    g_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    g_gl.PixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    return true;
}

// ---- Render targets -------------------------------------------------------------------------
void DeleteTarget(Target& t)
{
    if (t.fbo)
        g_gl.DeleteFramebuffers(1, &t.fbo);
    if (t.tex)
        g_gl.DeleteTextures(1, &t.tex);
    t = Target{};
}

GLuint NewTexture(GLint internal, int w, int h, GLenum format, GLenum type, GLint filter)
{
    GLuint tex = 0;
    g_gl.GenTextures(1, &tex);
    g_gl.BindTexture(GL_TEXTURE_2D, tex);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
    g_gl.TexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr);
    return tex;
}

bool MakeTarget(Target& t, int w, int h, GLint internal, GLenum format, GLenum type)
{
    DeleteTarget(t);
    t.w = w > 0 ? w : 1;
    t.h = h > 0 ? h : 1;
    t.tex = NewTexture(internal, t.w, t.h, format, type, GL_LINEAR);
    g_gl.GenFramebuffers(1, &t.fbo);
    g_gl.BindFramebuffer(kFramebuffer, t.fbo);
    g_gl.FramebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, t.tex, 0);
    return g_gl.CheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
}

// The two depth textures, in the format the current depthMode copies into (untried: the first).
bool MakeDepthTextures()
{
    const bool packed = g_res.depthMode <= 1;
    for (GLuint* tex : { &g_res.depth, &g_res.terrainDepth }) {
        if (*tex)
            g_gl.DeleteTextures(1, tex);
        *tex = packed ? NewTexture(kDepth24Stencil8, g_res.width, g_res.height, kDepthStencil, kUnsignedInt248, GL_NEAREST)
                      : NewTexture(kDepthComponent24, g_res.width, g_res.height, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_NEAREST);
        g_gl.TexParameteri(GL_TEXTURE_2D, kTextureCompareMode, GL_NONE);
    }
    const GLenum attachment = packed ? kDepthStencilAttachment : kDepthAttachment;
    const GLenum other = packed ? kDepthAttachment : kDepthStencilAttachment;
    g_gl.BindFramebuffer(kFramebuffer, g_res.captureFbo);
    g_gl.FramebufferTexture2D(kFramebuffer, other, GL_TEXTURE_2D, 0, 0);
    g_gl.FramebufferTexture2D(kFramebuffer, attachment, GL_TEXTURE_2D, g_res.depth, 0);
    const bool capture = g_gl.CheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
    g_gl.BindFramebuffer(kFramebuffer, g_res.terrainFbo);
    g_gl.FramebufferTexture2D(kFramebuffer, other, GL_TEXTURE_2D, 0, 0);
    g_gl.FramebufferTexture2D(kFramebuffer, attachment, GL_TEXTURE_2D, g_res.terrainDepth, 0);
    g_gl.DrawBuffer(GL_NONE);  // depth only (framebuffer object state, not the window's)
    g_gl.ReadBuffer(GL_NONE);
    const bool terrain = g_gl.CheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
    return capture && terrain;
}

bool MakeSizedTargets(int w, int h)
{
    g_res.width = w;
    g_res.height = h;
    const int hw = (w + 1) / 2, hh = (h + 1) / 2;
    bool ok = true;
    if (g_res.color)
        g_gl.DeleteTextures(1, &g_res.color);
    g_res.color = NewTexture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
    if (!g_res.captureFbo)
        g_gl.GenFramebuffers(1, &g_res.captureFbo);
    if (!g_res.terrainFbo)
        g_gl.GenFramebuffers(1, &g_res.terrainFbo);
    g_gl.BindFramebuffer(kFramebuffer, g_res.captureFbo);
    g_gl.FramebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, g_res.color, 0);
    if (g_res.depthMode < 0)
        g_res.depthMode = 0;  // nothing worked at the old size: try everything again
    MakeDepthTextures();
    g_res.depthVerified = g_res.colorVerified = false;

    ok = MakeTarget(g_res.hdr, w, h, kRgba16f, GL_RGBA, kHalfFloat) && ok;
    ok = MakeTarget(g_res.a, w, h, kRgba16f, GL_RGBA, kHalfFloat) && ok;
    ok = MakeTarget(g_res.b, w, h, kRgba16f, GL_RGBA, kHalfFloat) && ok;
    ok = MakeTarget(g_res.ldr, w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE) && ok;
    ok = MakeTarget(g_res.light, hw, hh, kRg8, kRg, GL_UNSIGNED_BYTE) && ok;
    ok = MakeTarget(g_res.dof, hw, hh, kRgba16f, GL_RGBA, kHalfFloat) && ok;
    ok = MakeTarget(g_res.rays, hw, hh, kRgba16f, GL_RGBA, kHalfFloat) && ok;
    for (int i = 0; i < kBloomLevels; ++i)
        ok = MakeTarget(g_res.bloom[i], w >> (i + 1), h >> (i + 1), kRgba16f, GL_RGBA, kHalfFloat) && ok;
    if (!ok)
        Log("postfx: could not create the %dx%d render targets", w, h);
    return ok;
}

bool EnsureResources(int w, int h)
{
    const HGLRC context = g_gl.GetCurrentContext();
    if (context != g_res.context) {
        g_res = Resources{};  // a new context: the old objects went with the old one
        g_res.context = context;
        DrainErrors();
        g_gl.IsEnabled(kAlphaTest);
        g_compatibility = g_gl.GetError() == GL_NO_ERROR;
    }
    if (g_res.failed)
        return false;

    if (!g_res.programs[0].id) {
        if (!BuildPrograms()) {
            Log("postfx: the shaders could not be compiled; they stay off (details above)");
            g_res.failed = true;
            return false;
        }
        const float triangle[] = { -1, -1, 3, -1, -1, 3 };
        g_gl.GenVertexArrays(1, &g_res.vao);
        g_gl.GenBuffers(1, &g_res.vbo);
        g_gl.BindVertexArray(g_res.vao);
        g_gl.BindBuffer(kArrayBuffer, g_res.vbo);
        g_gl.BufferData(kArrayBuffer, sizeof triangle, triangle, kStaticDraw);
        g_gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        g_gl.EnableVertexAttribArray(0);
        g_gl.ActiveTexture(kTexture0);
        CreateDetailTexture();
        for (Target& focus : g_res.focus)
            MakeTarget(focus, 1, 1, kR32f, kRed, GL_FLOAT);
    }
    if (w != g_res.width || h != g_res.height) {
        g_gl.ActiveTexture(kTexture0);
        if (!MakeSizedTargets(w, h)) {
            g_res.failed = true;
            return false;
        }
    }
    return true;
}

// ---- Copies out of the window ---------------------------------------------------------------
bool CaptureColor()
{
    while (g_res.colorMode >= 0) {
        if (g_res.colorMode == 0)
            g_res.colorMode = 1;
        if (!g_res.colorVerified)
            DrainErrors();
        g_gl.BindFramebuffer(kReadFramebuffer, 0);
        g_gl.ReadBuffer(GL_BACK);
        if (g_res.colorMode == 1) {
            g_gl.BindFramebuffer(kDrawFramebuffer, g_res.captureFbo);
            g_gl.BlitFramebuffer(0, 0, g_res.width, g_res.height, 0, 0, g_res.width, g_res.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        } else {
            g_gl.ActiveTexture(kTexture0);
            g_gl.BindTexture(GL_TEXTURE_2D, g_res.color);
            g_gl.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, g_res.width, g_res.height);
        }
        if (g_res.colorVerified || g_gl.GetError() == GL_NO_ERROR) {
            if (!g_res.colorVerified)
                Log("postfx: the window's image is read with a %s", g_res.colorMode == 1 ? "blit" : "texture copy");
            g_res.colorVerified = true;
            return true;
        }
        g_res.colorMode = g_res.colorMode == 1 ? 2 : -1;
    }
    Log("postfx: the window's image can't be read; shaders off");
    g_res.failed = true;
    return false;
}

// Copies the window's depth buffer into the texture attached to fbo (one of ours).
bool CaptureDepth(GLuint fbo, GLuint tex)
{
    while (g_res.depthMode >= 0) {
        if (g_res.depthMode == 0)
            g_res.depthMode = 1;  // the textures were made for it
        if (!g_res.depthVerified)
            DrainErrors();
        g_gl.BindFramebuffer(kReadFramebuffer, 0);
        if (g_res.depthMode <= 2) {
            g_gl.BindFramebuffer(kDrawFramebuffer, fbo);
            g_gl.BlitFramebuffer(0, 0, g_res.width, g_res.height, 0, 0, g_res.width, g_res.height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        } else {
            g_gl.ActiveTexture(kTexture0);
            g_gl.BindTexture(GL_TEXTURE_2D, tex);
            g_gl.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, g_res.width, g_res.height);
        }
        if (g_res.depthVerified || g_gl.GetError() == GL_NO_ERROR) {
            if (!g_res.depthVerified)
                Log("postfx: the depth buffer is read with a %s", g_res.depthMode <= 2 ? "blit" : "texture copy");
            g_res.depthVerified = true;
            return true;
        }
        if (++g_res.depthMode > 3) {
            g_res.depthMode = -1;
            Log("postfx: the depth buffer can't be read; AO, shadows, dirt detail, sky, DOF and motion blur are off");
            return false;
        }
        MakeDepthTextures();
        tex = fbo == g_res.captureFbo ? g_res.depth : g_res.terrainDepth;
    }
    return false;
}

// ---- Drawing helpers --------------------------------------------------------------------------
struct Matrices {
    float  proj[16] = {}, invProj[16] = {};
    float  texel[2] = {};
};
Matrices g_m;

Program& Use(ProgramId id)
{
    Program& p = g_res.programs[id];
    g_gl.UseProgram(p.id);
    g_gl.UniformMatrix4fv(p["u_proj"], 1, GL_FALSE, g_m.proj);
    g_gl.UniformMatrix4fv(p["u_invProj"], 1, GL_FALSE, g_m.invProj);
    g_gl.Uniform2f(p["u_texel"], g_m.texel[0], g_m.texel[1]);
    g_gl.Uniform1f(p["u_skyDepth"], 0.999999f);
    return p;
}

void Bind(Program& p, const char* name, int unit, GLuint tex)
{
    g_gl.ActiveTexture(kTexture0 + unit);
    g_gl.BindTexture(GL_TEXTURE_2D, tex);
    if (g_gl.BindSampler)
        g_gl.BindSampler(unit, 0);
    g_gl.Uniform1i(p[name], unit);
}

void Draw(const Target* target, int w, int h)
{
    g_gl.BindFramebuffer(kDrawFramebuffer, target ? target->fbo : 0);
    g_gl.Viewport(0, 0, w, h);
    g_gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

void Draw(const Target& target) { Draw(&target, target.w, target.h); }

void Vec(Program& p, const char* name, Vec3 v)
{
    g_gl.Uniform3f(p[name], static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z));
}

// ---- Matrix helpers (column-major, double) ----------------------------------------------------
bool Invert(const double m[16], double out[16])
{
    double inv[16];
    inv[0]  =  m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4]  = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8]  =  m[4] * m[9]  * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9]  * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1]  = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5]  =  m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9]  = -m[0] * m[9]  * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] =  m[0] * m[9]  * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2]  =  m[1] * m[6]  * m[15] - m[1] * m[7]  * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7]  - m[13] * m[3] * m[6];
    inv[6]  = -m[0] * m[6]  * m[15] + m[0] * m[7]  * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7]  + m[12] * m[3] * m[6];
    inv[10] =  m[0] * m[5]  * m[15] - m[0] * m[7]  * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7]  - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5]  * m[14] + m[0] * m[6]  * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6]  + m[12] * m[2] * m[5];
    inv[3]  = -m[1] * m[6]  * m[11] + m[1] * m[7]  * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9]  * m[2] * m[7]  + m[9]  * m[3] * m[6];
    inv[7]  =  m[0] * m[6]  * m[11] - m[0] * m[7]  * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8]  * m[2] * m[7]  - m[8]  * m[3] * m[6];
    inv[11] = -m[0] * m[5]  * m[11] + m[0] * m[7]  * m[9]  + m[4] * m[1] * m[11] - m[4] * m[3] * m[9]  - m[8]  * m[1] * m[7]  + m[8]  * m[3] * m[5];
    inv[15] =  m[0] * m[5]  * m[10] - m[0] * m[6]  * m[9]  - m[4] * m[1] * m[10] + m[4] * m[2] * m[9]  + m[8]  * m[1] * m[6]  - m[8]  * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-30)
        return false;
    for (int i = 0; i < 16; ++i)
        out[i] = inv[i] / det;
    return true;
}

void ToFloat(const double* in, float* out, int n)
{
    for (int i = 0; i < n; ++i)
        out[i] = static_cast<float>(in[i]);
}

// A view matrix's rotation applied to a world direction, and back.
Vec3 WorldToEye(const double v[16], Vec3 w)
{
    return { v[0] * w.x + v[4] * w.y + v[8] * w.z, v[1] * w.x + v[5] * w.y + v[9] * w.z, v[2] * w.x + v[6] * w.y + v[10] * w.z };
}
Vec3 EyeToWorld(const double v[16], Vec3 e)
{
    return { v[0] * e.x + v[1] * e.y + v[2] * e.z, v[4] * e.x + v[5] * e.y + v[6] * e.z, v[8] * e.x + v[9] * e.y + v[10] * e.z };
}
Vec3 CameraPosition(const double v[16])
{
    return EyeToWorld(v, Vec3{ -v[12], -v[13], -v[14] });
}

Vec3 WorldUp()
{
    const Config& cfg = GetConfig();
    return Normalize(Vec3{ cfg.worldUp[0], cfg.worldUp[1], cfg.worldUp[2] });
}

const double* BestRawView()
{
    return g_frame.haveView ? g_frame.raw : (g_haveLastRaw ? g_lastRaw : nullptr);
}

// The game's sun from a light uniform in its eye space.
bool SunFromEye(const double raw[16], const float* eye, Vec3& sun)
{
    const Vec3 e{ eye[0], eye[1], eye[2] };
    if (Length(e) < 1e-6)
        return false;
    sun = Normalize(EyeToWorld(raw, e)) * (GetConfig().invertLight ? -1.0 : 1.0);
    return true;
}

timeofday::Sky CurrentSky()
{
    const Config& cfg = GetConfig();
    return cfg.timeOfDay >= 0 ? timeofday::Compute(cfg.timeOfDay, g_gameSun, WorldUp(), cfg.nightBrightness)
                              : timeofday::Neutral(g_gameSun, WorldUp());
}

// What lights the scene: the game's sun, or ours when the time of day turns it.
Vec3 SceneLight(const timeofday::Sky& sky)
{
    const Config& cfg = GetConfig();
    return (cfg.timeOfDay >= 0 && cfg.moveSun) ? sky.light : g_gameSun;
}

// ---- The sun's camera for the shadow map ---------------------------------------------------
// A square of 2 x ShadowDistance around (and mostly ahead of) the camera, seen straight along
// the light. Its centre moves in whole texels, so shadow edges don't crawl as the camera moves.
constexpr double kShadowDepth = 100.0;  // m on either side of the centre, along the light

void SetupShadowFrame()
{
    const Config& cfg = GetConfig();
    g_shadow.setup = true;
    g_shadow.usable = false;
    if (!cfg.shadowMap || cfg.shadows <= 0.0 || !g_frame.haveView || (!g_haveGameSun && cfg.timeOfDay < 0))
        return;
    const Vec3 up = WorldUp();
    const Vec3 light = Normalize(SceneLight(CurrentSky()));
    if (Dot(light, up) < 0.05)
        return;  // the light is on the horizon (or under it): shadows would be endless
    const double* V = g_frame.rendered;
    Vec3 ahead = EyeToWorld(V, Vec3{ 0, 0, -1 });
    ahead = ahead - up * Dot(ahead, up);
    ahead = Length(ahead) > 1e-3 ? Normalize(ahead) : Vec3{};
    const double half = cfg.shadowDistance;
    const Vec3 centre = CameraPosition(V) + ahead * (half * 0.45);

    const Vec3 z = light;  // the sun's camera looks down -z, at the scene
    const Vec3 side = std::fabs(Dot(z, up)) > 0.99 ? Vec3{ 1, 0, 0 } : up;
    const Vec3 x = Normalize(Cross(side, z));
    const Vec3 y = Cross(z, x);
    const double texel = 2.0 * half / cfg.shadowMapSize;
    const double cx = std::floor(Dot(centre, x) / texel) * texel;
    const double cy = std::floor(Dot(centre, y) / texel) * texel;
    const double cz = Dot(centre, z);
    const double lightView[16] = { x.x, y.x, z.x, 0,  x.y, y.y, z.y, 0,  x.z, y.z, z.z, 0,  -cx, -cy, -cz, 1 };
    double invView[16];
    if (!AffineInverse(V, invView))
        return;
    MatMul(lightView, invView, g_shadow.toLight);
    const float projection[16] = { static_cast<float>(1.0 / half), 0, 0, 0,  0, static_cast<float>(1.0 / half), 0, 0,
                                   0, 0, static_cast<float>(-1.0 / kShadowDepth), 0,  0, 0, 0, 1 };
    for (int i = 0; i < 16; ++i)
        g_shadow.projection[i] = projection[i];
    g_shadow.half = half;
    g_shadow.usable = true;
}

// ---- The headlight: on the bike's front plate ----------------------------------------------
Vec3 TransformPoint(const double m[16], Vec3 p)
{
    return { m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
             m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] };
}
Vec3 TransformDirection(const double m[16], Vec3 d)
{
    return { m[0] * d.x + m[4] * d.y + m[8] * d.z, m[1] * d.x + m[5] * d.y + m[9] * d.z, m[2] * d.x + m[6] * d.y + m[10] * d.z };
}

// ---- Skeletons: your rider and your bike ------------------------------------------------------
// Every skinned draw of the frame with its bones and the u_modelview it's drawn with: riders are
// drawn with the bare view (their bones are world positions), bikes with a model of their own (their
// bones are in it). Both end up in world space here.
constexpr int kSkeletonBones = 32;
struct SkeletonRecord {
    unsigned program = 0;
    float    viewModel[16] = {};
    int      bones = 0, torso = -1;
    bool     rider = false, transposed = false;
    float    position[kSkeletonBones * 3] = {};
    float    rotation[kSkeletonBones * 9] = {};
};
std::vector<SkeletonRecord> g_skeletons;

// World transform of a record's model (camera -> world times its View * Model).
bool RecordWorld(const SkeletonRecord& rec, double world[16])
{
    double invRaw[16], vm[16];
    if (!AffineInverse(g_frame.raw, invRaw))
        return false;
    for (int i = 0; i < 16; ++i)
        vm[i] = rec.viewModel[i];
    MatMul(invRaw, vm, world);
    return true;
}

Vec3 BonePosition(const SkeletonRecord& rec, const double world[16], int b)
{
    return TransformPoint(world, Vec3{ rec.position[b * 3], rec.position[b * 3 + 1], rec.position[b * 3 + 2] });
}

// The bone's own axes in world space (columns of its rotation, turned by the model's).
void BoneRotation(const SkeletonRecord& rec, const double world[16], int b, Vec3 axes[3])
{
    const float* r = rec.rotation + b * 9;
    for (int c = 0; c < 3; ++c) {
        const Vec3 col = rec.transposed ? Vec3{ r[c], r[3 + c], r[6 + c] } : Vec3{ r[c * 3], r[c * 3 + 1], r[c * 3 + 2] };
        axes[c] = TransformDirection(world, col);
    }
}

// This frame's objects near the camera (View * Model as the game sent it), and what the bike
// tracker taught about your bike while it was locked in first person: which program draws it,
// and where the rider's eyes and the rider sit on it, in its own space. In third person (or
// with the camera mod off) the bike is the object that carries your rider in that same place.
struct SceneObject { unsigned program; float viewModel[16]; };
std::vector<SceneObject> g_objects;

struct BikeModel {
    bool     learned = false;             // the bike tracker taught it this session
    bool     loaded = false;              // read from the ini's [Learned] section
    unsigned program = 0;                 // 0: any
    // MX Simulator's bikes: the origin sits about 1.5 m under the ground, the bike faces -Z.
    Vec3     eye{ 0.0, 3.05, -1.0 };      // the rider's eyes at rest, in the bike's space
    Vec3     rider{ 0.0, 2.4, -0.85 };    // the middle of the rider and bike, in the bike's space
    Vec3     forward{ 0.0, 0.0, -1.0 };   // the bike model's own axes
    Vec3     up{ 0.0, 1.0, 0.0 };
    double   samples = 0;                 // seconds learnt
    double   savedAt = -1e9;              // when it was last written to the ini
};
BikeModel g_bikeModel;
bool      g_thirdPerson = false;          // the camera is away from your rider (with some hysteresis)

// Your bike this frame (world transform), however it was found.
struct BikeNow { bool found = false; int how = 0; double model[16] = {}; };
BikeNow g_bikeNow;

// ---- Your bike's wheels ----------------------------------------------------------------------
// Every bone (or rigid part) drawn under your rider is followed from frame to frame; the two
// that keep turning, one ahead of the other, are your wheels (their bones sit at the axles). The
// tires touch the ground a wheel's radius below them, along the bike's lean.
struct BoneTrack {
    bool     used = false, seenNow = false;
    unsigned program = 0;
    Vec3     rel;              // where it is from your rider: along the bike, across it, up
    Vec3     relMean;          // the same, averaged (a wheel whose origin is off its axle circles it)
    double   wander = 0;       // how far it strays from that average (m)
    Vec3     axes[3];          // its own axes (world), last seen
    Vec3     world;            // its position this frame
    double   spin = 0;         // radians turned in about the last second
    double   lastSeen = -1;
    double   total = 0;        // radians turned in all
};
BoneTrack g_boneTracks[96];

struct Wheel {
    bool   on = false, logged = false;
    int    track = -1;
    Vec3   axle;              // world
    Vec3   axes[3];           // the wheel's own axes (world): they turn with it
    int    axleAxis = 0;      // which of them is the axle
    double radius = 0.35;     // m, to the outside of the tire
    Vec3   contact;           // world
    bool   haveLast = false;  // the last contact stamped into the tire marks
    double lastU = 0, lastV = 0;
};
Wheel  g_wheels[2];            // 0 rear, 1 front
double g_movingWithoutWheels = 0;
bool   g_loggedNoWheels = false;

// Airborne: the bike falls freely (its height accelerates downwards at about 1 g).
struct Air {
    bool   haveLast = false, haveVelocity = false, airborne = false;
    double lastHeight = 0, velocity = 0, accel = 0, freeFall = 0;
};
Air g_air;

struct OwnRider {
    bool   seen = false;           // found this frame
    Vec3   position;               // world: the middle of the rider and bike
    double nearestBone = 1e9;      // m from the camera to its nearest bone (first person: tiny)
    bool   haveLast = false;
    Vec3   last;
    Vec3   velocity;               // smoothed, m/s
    bool   haveHeading = false;
    Vec3   heading;                // unit, level: the way the bike points
    bool   haveLean = false;
    Vec3   lean;                   // unit: the rider's (and so the bike's) up, leaning in corners
};
OwnRider g_rider;

void FindOwnRider(double dt)
{
    OwnRider& r = g_rider;
    r.seen = false;
    if (g_skeletons.empty() || !g_frame.haveView)
        return;
    const Vec3 eye = CameraPosition(g_frame.raw);
    // Your rider: the one whose nearest bone is nearest the camera.
    int best = -1;
    double bestDistance = 40.0;  // further than this, it isn't you (the far chase camera sits ~12 m back)
    std::vector<Vec3> centroids(g_skeletons.size());
    for (size_t i = 0; i < g_skeletons.size(); ++i) {
        const SkeletonRecord& rec = g_skeletons[i];
        double world[16];
        if (!rec.rider || !RecordWorld(rec, world))
            continue;
        Vec3 sum;
        for (int b = 0; b < rec.bones; ++b) {
            const Vec3 bone = BonePosition(rec, world, b);
            sum = sum + bone;
            const double d = Length(bone - eye);
            if (d < bestDistance) {
                bestDistance = d;
                best = static_cast<int>(i);
            }
        }
        centroids[i] = sum / rec.bones;
    }
    if (best < 0)
        return;
    Vec3 sum;
    int n = 0;
    for (size_t i = 0; i < g_skeletons.size(); ++i) {
        if (g_skeletons[i].rider && Length(centroids[i] - centroids[best]) < 1.2) {
            sum = sum + centroids[i];
            ++n;
        }
    }
    const Vec3 position = sum / n;
    r.seen = true;
    r.nearestBone = bestDistance;
    if (r.haveLast && dt > 0.0) {
        const Vec3 step = position - r.last;
        if (Length(step) > 3.0) {  // a respawn or a camera cut: start over
            r.velocity = Vec3{};
            r.haveHeading = false;
        } else {
            const double a = 1.0 - std::exp(-dt * 6.0);
            r.velocity = r.velocity + (step / dt - r.velocity) * a;
        }
    }
    r.last = position;
    r.haveLast = true;
    r.position = position;
    // Which way the bike points, level: the way your rider's torso faces (known standing still,
    // and it turns with the bike in slides), or else the way it's travelling. Never up or down:
    // a drop onto the start gate or a landing would aim the light at the ground.
    const Vec3 worldUp = WorldUp();
    const auto Level = [&](Vec3 v, Vec3& out) {
        v = v - worldUp * Dot(v, worldUp);
        if (Length(v) < 1e-3)
            return false;
        out = Normalize(v);
        return true;
    };
    Vec3 target;
    bool haveTarget = false;
    double rate = 10.0;
    {
        const SkeletonRecord& rec = g_skeletons[best];
        double world[16];
        Vec3 axes[3];
        Vec3 forward;
        if (rec.torso >= 0 && RecordWorld(rec, world)) {
            BoneRotation(rec, world, rec.torso, axes);
            forward = axes[2] * -1.0;
        }
        if (rec.torso >= 0 && Level(forward, target)) {
            haveTarget = true;
            // The torso's up, without its lean forward / back: the bike's lean.
            Vec3 lean = axes[1] - target * Dot(axes[1], target);
            if (Length(lean) > 0.3 && Dot(lean, worldUp) > 0.3) {
                lean = Normalize(lean);
                const double a = r.haveLean ? 1.0 - std::exp(-dt * 12.0) : 1.0;
                r.lean = Normalize(r.lean + (lean - r.lean) * a);
                r.haveLean = true;
            }
        }
    }
    if (!haveTarget) {
        Vec3 flatVelocity;
        const double flatSpeed = Level(r.velocity, flatVelocity) ? Length(r.velocity - worldUp * Dot(r.velocity, worldUp)) : 0.0;
        if (flatSpeed > 3.0) {  // slower than this the heading is held (at the gate, after a crash)
            target = flatVelocity;
            haveTarget = true;
            rate = 8.0 * Clamp((flatSpeed - 3.0) / 3.0, 0.0, 1.0);
        }
    }
    if (haveTarget) {
        if (!r.haveHeading || Dot(r.heading, target) < 0.0) {
            r.heading = target;
            r.haveHeading = true;
        } else {
            r.heading = Normalize(r.heading + (target - r.heading) * (1.0 - std::exp(-dt * rate)));
        }
    }
}

// World position of the front plate, the bike's forward and its up. First person with the
// helmet cam: from the bike tracker, placed from where the rider's eyes sit on the bike.
// Otherwise (third person, helmet cam off, replays): from your rider's skeleton and direction
// of travel. Before either is known it rides on the game's camera.
int g_headlightMode = -1;  // how the headlight was last placed (for the log)

void NoteHeadlightMode(int mode, const Vec3& position)
{
    if (mode == g_headlightMode)
        return;
    g_headlightMode = mode;
    static const char* const kModes[] = { "on your bike (bike tracker, first person)", "on your bike (found under your rider)",
                                          "on your rider (bike not found)", "on the camera (no rider found yet)" };
    const Vec3 offset = position - CameraPosition(g_frame.raw);
    Log("postfx: headlight placed %s, %.2f m from the camera (rider's nearest bone %.2f m away; bike %s)", kModes[mode],
        Length(offset), g_rider.seen ? g_rider.nearestBone : -1.0,
        g_bikeModel.learned ? "learnt in first person" : "not learnt yet: ride in first person for a few seconds to teach it");
}

// The plate on the bike, in the bike's own space.
Vec3 PlateOnBike()
{
    const Config& cfg = GetConfig();
    return g_bikeModel.eye + g_bikeModel.forward * cfg.headlightForward + g_bikeModel.up * cfg.headlightHeight;
}

void PoseFromBike(const double model[16], Vec3& position, Vec3& forward, Vec3& up)
{
    position = TransformPoint(model, PlateOnBike());
    forward = Normalize(TransformDirection(model, g_bikeModel.forward));
    up = Normalize(TransformDirection(model, g_bikeModel.up));
}

// First person with the bike tracker locked: learn where the eyes and the rider sit on the bike.
void LoadLearnedBike()
{
    BikeModel& b = g_bikeModel;
    if (b.loaded)
        return;
    b.loaded = true;
    double v[3];
    if (ReadLearned("BikeEye", v))
        b.eye = Vec3{ v[0], v[1], v[2] };
    if (ReadLearned("BikeRider", v))
        b.rider = Vec3{ v[0], v[1], v[2] };
    ForgetLearned("FrontAxle");  // from an earlier version: wheels are placed from your rider now
    ForgetLearned("RearAxle");
    Log("postfx: your bike: eyes at (%.2f, %.2f, %.2f) on it (%s)", b.eye.x, b.eye.y, b.eye.z,
        ReadLearned("BikeEye", v) ? "learnt" : "MX Simulator's usual place until it's learnt in first person");
}

void SaveLearnedBike(double now, bool force)
{
    BikeModel& b = g_bikeModel;
    if (!force && now - b.savedAt < 20.0)
        return;
    b.savedAt = now;
    const double eye[3] = { b.eye.x, b.eye.y, b.eye.z }, rider[3] = { b.rider.x, b.rider.y, b.rider.z };
    if (b.learned) {
        WriteLearned("BikeEye", eye);
        WriteLearned("BikeRider", rider);
    }
}

// First person with the bike tracker locked: learn where the eyes and the rider sit on the bike.
void LearnBike(const double model[16], const bike::Pose& pose, double dt)
{
    double inv[16];
    if (!AffineInverse(model, inv))
        return;
    BikeModel& b = g_bikeModel;
    const Vec3 eye = TransformPoint(inv, CameraPosition(g_frame.raw));
    const double a = b.learned ? 1.0 - std::exp(-dt / 3.0) : 1.0;  // a few seconds' average: no head bob
    b.eye = b.eye + (eye - b.eye) * a;
    if (g_rider.seen)
        b.rider = b.rider + (TransformPoint(inv, g_rider.position) - b.rider) * (b.learned ? a : 1.0);
    b.forward = Vec3{ pose.forward[0], pose.forward[1], pose.forward[2] };
    b.up = Vec3{ pose.up[0], pose.up[1], pose.up[2] };
    b.program = pose.program;
    b.samples += dt;
    if (!b.learned) {
        b.learned = true;
        Log("postfx: headlight learnt your bike (program %u): eyes at (%.2f, %.2f, %.2f) in its space", b.program, eye.x,
            eye.y, eye.z);
    }
}

// Third person (or the camera mod off): the object that carries your rider where the bike does.
// Once found it's followed from frame to frame (the rider leans and shifts on it, the bike
// doesn't jump).
bool   g_haveLastBike = false;
Vec3   g_lastBikeOrigin;
unsigned g_lastBikeProgram = 0;

bool FindBike(double model[16])
{
    if (!g_rider.seen || g_objects.empty()) {
        g_haveLastBike = false;
        return false;
    }
    double invRaw[16];
    if (!AffineInverse(g_frame.raw, invRaw))
        return false;
    const Vec3 worldUp = WorldUp();
    const BikeModel& b = g_bikeModel;
    double bestError = 1e9;
    bool found = false;
    for (int pass = 0; pass < 2 && !found; ++pass) {
        const bool follow = pass == 0;
        if (follow && !g_haveLastBike)
            continue;
        bestError = follow ? 1.6 : (b.learned || b.loaded ? 0.7 : 1.0);
        for (const SceneObject& o : g_objects) {
            if (follow ? o.program != g_lastBikeProgram : (b.program && o.program != b.program))
                continue;
            double vm[16], world[16];
            for (int i = 0; i < 16; ++i)
                vm[i] = o.viewModel[i];
            MatMul(invRaw, vm, world);
            if (Dot(Normalize(TransformDirection(world, b.up)), worldUp) < 0.5)
                continue;  // not upright (a wheel, a fork)
            const Vec3 origin{ world[12], world[13], world[14] };
            if (follow && Length(origin - g_lastBikeOrigin) > 2.0)
                continue;
            if (!follow && g_rider.haveHeading) {
                Vec3 f = TransformDirection(world, b.forward);
                f = f - worldUp * Dot(f, worldUp);
                if (Length(f) < 1e-3 || Dot(Normalize(f), g_rider.heading) < 0.7)
                    continue;
            }
            const double error = Length(TransformPoint(world, b.rider) - g_rider.position);
            if (error < bestError) {
                bestError = error;
                std::copy(world, world + 16, model);
                found = true;
                g_lastBikeProgram = o.program;
            }
        }
    }
    g_haveLastBike = found;
    if (found)
        g_lastBikeOrigin = Vec3{ model[12], model[13], model[14] };
    return found;
}

// Your bike this frame: first person with the bike tracker locked (teaching the headlight
// where things sit on it), else the object under your rider.
void UpdateBike(double dt)
{
    const Config& cfg = GetConfig();
    LoadLearnedBike();
    g_bikeNow.found = false;
    double invRaw[16];
    if (!g_frame.haveView || !AffineInverse(g_frame.raw, invRaw))
        return;
    // First person: the camera is at your rider's head (its nearest bone ~0.7 m away); third
    // person: metres behind. Hysteresis, so it never flips back and forth.
    if (g_rider.seen) {
        if (g_rider.nearestBone > 2.0)
            g_thirdPerson = true;
        else if (g_rider.nearestBone < 1.4)
            g_thirdPerson = false;
    }
    const bike::Pose& pose = bike::Current();
    if (cfg.enabled && !g_thirdPerson && pose.tracked) {
        MatMul(invRaw, pose.cameraSpace, g_bikeNow.model);
        LearnBike(g_bikeNow.model, pose, dt);
        g_bikeNow.found = true;
        g_bikeNow.how = 0;
    } else if (FindBike(g_bikeNow.model)) {
        g_bikeNow.found = true;
        g_bikeNow.how = 1;
    }
}

Vec3 Column(const double m[16], int c) { return Vec3{ m[c * 4], m[c * 4 + 1], m[c * 4 + 2] }; }
Vec3 Origin(const double m[16]) { return Vec3{ m[12], m[13], m[14] }; }

void UpdateWheels(double now, double dt)
{
    const Vec3 worldUp = WorldUp();
    for (Wheel& w : g_wheels)
        w.on = false;
    for (BoneTrack& t : g_boneTracks) {
        t.seenNow = false;
        if (t.used && now - t.lastSeen > 3.0)
            t = BoneTrack{};
    }
    if (!g_rider.seen || !g_rider.haveHeading)
        return;
    const Vec3 forward = g_rider.heading, across = Normalize(Cross(forward, worldUp));
    const Vec3 up = g_rider.haveLean ? g_rider.lean : worldUp;
    // Rigid parts drawn by any program count too, as one bone at their origin.
    std::vector<SkeletonRecord> parts;
    const Vec3 riderEye = TransformPoint(g_frame.raw, g_rider.position);
    for (const SceneObject& o : g_objects) {
        const Vec3 at{ o.viewModel[12], o.viewModel[13], o.viewModel[14] };
        if (Length(at - riderEye) > 3.5)
            continue;  // not under your rider
        SkeletonRecord rec;
        rec.program = o.program;
        rec.bones = 1;
        std::copy(o.viewModel, o.viewModel + 16, rec.viewModel);
        rec.rotation[0] = rec.rotation[4] = rec.rotation[8] = 1.0f;
        parts.push_back(rec);
    }
    std::vector<const SkeletonRecord*> all;
    for (const SkeletonRecord& rec : parts)
        all.push_back(&rec);
    for (const SkeletonRecord& rec : g_skeletons)
        all.push_back(&rec);
    for (const SkeletonRecord* recPointer : all) {
        const SkeletonRecord& rec = *recPointer;
        double world[16];
        if (rec.rider || !RecordWorld(rec, world))
            continue;
        for (int b = 0; b < rec.bones; ++b) {
            const Vec3 at = BonePosition(rec, world, b);
            const Vec3 d = at - g_rider.position;
            const Vec3 rel{ Dot(d, forward), Dot(d, across), Dot(d, worldUp) };
            if (std::fabs(rel.x) > 1.8 || std::fabs(rel.y) > 0.5 || rel.z > 0.1 || rel.z < -1.6)
                continue;
            Vec3 axes[3];
            BoneRotation(rec, world, b, axes);
            BoneTrack* match = nullptr;
            double nearest = 0.5;
            for (BoneTrack& t : g_boneTracks) {
                if (t.used && !t.seenNow && t.program == rec.program && Length(t.rel - rel) < nearest) {
                    nearest = Length(t.rel - rel);
                    match = &t;
                }
            }
            if (!match) {
                for (BoneTrack& t : g_boneTracks)
                    if (!t.used) {
                        match = &t;
                        break;
                    }
                if (!match)
                    continue;
                *match = BoneTrack{};
                match->used = true;
                match->program = rec.program;
                match->rel = rel;
                match->relMean = rel;
                for (int k = 0; k < 3; ++k)
                    match->axes[k] = axes[k];
            }
            BoneTrack& t = *match;
            // How far it turned since it was last seen: the angle of the turn from its old axes.
            double turn = 0;
            if (t.lastSeen >= 0 && now - t.lastSeen < 0.25) {
                double trace = 0;
                for (int k = 0; k < 3; ++k)
                    trace += Dot(axes[k], t.axes[k]);
                turn = std::acos(Clamp((trace - 1.0) * 0.5, -1.0, 1.0));
            }
            t.spin = t.spin * std::exp(-dt / 0.8) + turn;
            t.total += turn;
            t.rel = t.rel + (rel - t.rel) * 0.5;
            t.relMean = t.relMean + (rel - t.relMean) * 0.08;
            t.wander = t.wander + (Length(rel - t.relMean) - t.wander) * 0.1;
            for (int k = 0; k < 3; ++k)
                t.axes[k] = axes[k];
            t.world = at;
            t.lastSeen = now;
            t.seenNow = true;
        }
    }
    // The wheels: keep the ones found while they're there, else the two that turn the most, one
    // ahead of the other.
    const auto Spinning = [&](int i) { return g_boneTracks[i].used && g_boneTracks[i].seenNow && g_boneTracks[i].spin > 2.0; };
    bool keep = true;
    for (const Wheel& w : g_wheels)
        keep = keep && w.track >= 0 && g_boneTracks[w.track].used && g_boneTracks[w.track].seenNow;
    if (!keep) {
        int bestA = -1, bestB = -1;
        double bestScore = 0;
        for (int a = 0; a < 96; ++a) {
            if (!Spinning(a))
                continue;
            for (int b = 0; b < 96; ++b) {
                if (b == a || !Spinning(b) || g_boneTracks[a].relMean.x - g_boneTracks[b].relMean.x < 0.8)
                    continue;  // a ahead of b by a wheelbase or so
                const double score = g_boneTracks[a].spin + g_boneTracks[b].spin;
                if (score > bestScore) {
                    bestScore = score;
                    bestA = a;
                    bestB = b;
                }
            }
        }
        if (bestA >= 0) {
            g_wheels[1].track = bestA;
            g_wheels[0].track = bestB;
            for (int i = 0; i < 2; ++i) {
                Wheel& w = g_wheels[i];
                const BoneTrack& t = g_boneTracks[w.track];
                if (!w.logged) {
                    w.logged = true;
                    Log("postfx: found your bike's %s wheel (program %u): %.2f m %s your rider, %.2f m below", i ? "front" : "rear",
                        t.program, std::fabs(t.rel.x), t.rel.x >= 0 ? "ahead of" : "behind", -t.rel.z);
                }
            }
        }
    }
    bool any = false;
    for (int i = 0; i < 2; ++i) {
        Wheel& w = g_wheels[i];
        if (w.track < 0 || !g_boneTracks[w.track].used || !g_boneTracks[w.track].seenNow)
            continue;
        const BoneTrack& t = g_boneTracks[w.track];
        w.on = true;
        any = true;
        // Its axle: where the bone is, or (for a wheel model whose origin is off its axle) the middle
        // of the circle it goes round.
        w.axle = t.wander < 0.06 ? t.world
                                 : g_rider.position + forward * t.relMean.x + across * t.relMean.y + worldUp * t.relMean.z;
        double most = -1;
        for (int k = 0; k < 3; ++k) {
            w.axes[k] = Normalize(t.axes[k]);
            const double a = std::fabs(Dot(w.axes[k], across));
            if (a > most) {
                most = a;
                w.axleAxis = k;
            }
        }
        w.radius = i == 1 ? 0.355 : 0.345;
        w.contact = w.axle - up * w.radius;
    }
    // Riding without wheels found: say so once, with what was seen (for bug reports).
    g_movingWithoutWheels = (!any && Length(g_rider.velocity) > 3.0) ? g_movingWithoutWheels + dt : 0.0;
    if (g_movingWithoutWheels > 15.0 && !g_loggedNoWheels) {
        g_loggedNoWheels = true;
        int seen = 0;
        for (const BoneTrack& t : g_boneTracks) {
            if (!t.used)
                continue;
            ++seen;
            Log("postfx: part under your rider: program %u, %.2f m along, %.2f across, %.2f up, turned %.1f rad", t.program,
                t.rel.x, t.rel.y, t.rel.z, t.total);
        }
        Log("postfx: your bike's wheels weren't found (%d part(s) under your rider, %d skinned draw(s) near the camera): no tire "
            "marks or tire dirt (your rider %.1f m from the camera)", seen, static_cast<int>(g_skeletons.size()),
            g_rider.nearestBone);
    }
}

// In the air: the bike's height accelerates downwards at about 1 g for a moment.
void UpdateAir(double dt)
{
    Air& a = g_air;
    if (!g_rider.seen || dt <= 0.0) {
        a = Air{};
        return;
    }
    const double height = Dot(g_rider.position, WorldUp());
    if (a.haveLast) {
        const double velocity = (height - a.lastHeight) / dt;
        if (std::fabs(height - a.lastHeight) > 3.0) {
            a = Air{};
        } else {
            if (a.haveVelocity)
                a.accel += ((velocity - a.velocity) / dt - a.accel) * (1.0 - std::exp(-dt / 0.06));
            a.velocity = velocity;
            a.haveVelocity = true;
        }
    }
    a.lastHeight = height;
    a.haveLast = true;
    a.freeFall = a.accel < -0.7 * kGravity ? a.freeFall + dt : 0.0;
    if (a.freeFall > 0.12)
        a.airborne = true;
    else if (a.accel > -0.4 * kGravity)
        a.airborne = false;
}

// ---- Tire marks ------------------------------------------------------------------------------
// A map of the ground around you (4096 x 4096 texels of 4 cm: 164 m across) that the tires
// press grooves into. It wraps around as you ride: the strip that leaves on one side comes back
// cleared on the other.
constexpr int    kMarkSize  = 4096;
constexpr double kMarkTexel = 0.04;
struct MarkWindow {
    bool      set = false;
    long long originU = 0, originV = 0;  // texel coordinates of the window's corner
};
MarkWindow g_markWindow;

void PlaneAxes(Vec3& u, Vec3& v)
{
    const Vec3 up = WorldUp();
    u = Normalize(Cross(up, std::fabs(up.z) < 0.9 ? Vec3{ 0, 0, 1 } : Vec3{ 1, 0, 0 }));
    v = Normalize(Cross(u, up));
}

int MarkWrap(long long t) { return static_cast<int>(((t % kMarkSize) + kMarkSize) % kMarkSize); }

bool EnsureMarks()
{
    if (g_res.marks.tex)
        return true;
    GLint active = 0, bound = 0;
    g_gl.GetIntegerv(kActiveTexture, &active);
    g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    const bool ok = MakeTarget(g_res.marks, kMarkSize, kMarkSize, kR8, kRed, GL_UNSIGNED_BYTE);
    g_gl.BindTexture(GL_TEXTURE_2D, g_res.marks.tex);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    g_gl.BindTexture(GL_TEXTURE_2D, bound);
    g_gl.ActiveTexture(active);
    g_res.marksCleared = false;
    Log("postfx: tire marks map %dx%d %s", kMarkSize, kMarkSize, ok ? "ready" : "could not be made");
    if (!ok)
        DeleteTarget(g_res.marks);
    return ok;
}

// Writes value (blend off) or presses a groove (max blend) between a and b, in the map's own
// texels (wrapped), over the rectangle x0, y0, w, h.
void StampRect(int x0, int y0, int w, int h, float ax, float ay, float bx, float by, float halfWidth, float strength)
{
    if (w <= 0 || h <= 0)
        return;
    Program& p = g_res.programs[kStamp];
    g_gl.Uniform2f(p["u_a"], ax, ay);
    g_gl.Uniform2f(p["u_b"], bx, by);
    g_gl.Uniform1f(p["u_half"], halfWidth);
    g_gl.Uniform1f(p["u_strength"], strength);
    g_gl.Scissor(x0, y0, w, h);
    Draw(g_res.marks);
}

void ClearMarks(int x0, int y0, int w, int h)
{
    StampRect(x0, y0, w, h, -1e6f, -1e6f, -1e6f, -1e6f, 1.0f, 0.0f);
}

// A groove from the last contact to this one (texel coordinates, unwrapped), drawn once per
// wrap-around copy it touches.
void Groove(double au, double av, double bu, double bv, double halfWidth, double strength)
{
    const MarkWindow& m = g_markWindow;
    const double pu = MarkWrap(m.originU), pv = MarkWrap(m.originV);
    const double ax = au - m.originU + pu, ay = av - m.originV + pv;
    const double bx = bu - m.originU + pu, by = bv - m.originV + pv;
    const double pad = halfWidth + 2.0;
    const double minX = std::fmin(ax, bx) - pad, maxX = std::fmax(ax, bx) + pad;
    const double minY = std::fmin(ay, by) - pad, maxY = std::fmax(ay, by) + pad;
    for (int sx = 0; sx < 2; ++sx) {
        for (int sy = 0; sy < 2; ++sy) {
            const double ox = sx * kMarkSize, oy = sy * kMarkSize;
            const int x0 = static_cast<int>(std::floor(minX - ox)), x1 = static_cast<int>(std::ceil(maxX - ox));
            const int y0 = static_cast<int>(std::floor(minY - oy)), y1 = static_cast<int>(std::ceil(maxY - oy));
            const int cx0 = std::max(x0, 0), cy0 = std::max(y0, 0);
            const int cx1 = std::min(x1, kMarkSize), cy1 = std::min(y1, kMarkSize);
            if (cx1 <= cx0 || cy1 <= cy0)
                continue;
            StampRect(cx0, cy0, cx1 - cx0, cy1 - cy0, static_cast<float>(ax - ox), static_cast<float>(ay - oy),
                      static_cast<float>(bx - ox), static_cast<float>(by - oy), static_cast<float>(halfWidth),
                      static_cast<float>(strength));
        }
    }
}

// Clears the texel columns (axis 0) or rows (axis 1) [from, from + count) of the window, wrapped.
void ClearBand(int axis, long long from, long long count)
{
    if (count >= kMarkSize) {
        ClearMarks(0, 0, kMarkSize, kMarkSize);
        return;
    }
    const int start = MarkWrap(from);
    const int first = static_cast<int>(std::min<long long>(count, kMarkSize - start));
    const int second = static_cast<int>(count - first);
    if (axis == 0) {
        ClearMarks(start, 0, first, kMarkSize);
        ClearMarks(0, 0, second, kMarkSize);
    } else {
        ClearMarks(0, start, kMarkSize, first);
        ClearMarks(0, 0, kMarkSize, second);
    }
}

// Moves the window with you and presses this frame's grooves. Returns whether the map is usable.
bool UpdateMarks()
{
    const Config& cfg = GetConfig();
    if (cfg.tireMarks <= 0.0 || !g_frame.haveView || !EnsureMarks())
        return false;
    Program& p = Use(kStamp);
    (void)p;
    g_gl.Enable(GL_SCISSOR_TEST);
    g_gl.Disable(GL_BLEND);
    if (!g_res.marksCleared) {
        ClearMarks(0, 0, kMarkSize, kMarkSize);
        g_res.marksCleared = true;
        g_markWindow.set = false;
    }
    Vec3 u, v;
    PlaneAxes(u, v);
    const Vec3 eye = CameraPosition(g_frame.raw);
    const long long cu = static_cast<long long>(std::floor(Dot(eye, u) / kMarkTexel));
    const long long cv = static_cast<long long>(std::floor(Dot(eye, v) / kMarkTexel));
    MarkWindow& m = g_markWindow;
    if (!m.set) {
        m.originU = cu - kMarkSize / 2;
        m.originV = cv - kMarkSize / 2;
        m.set = true;
        ClearMarks(0, 0, kMarkSize, kMarkSize);
    }
    // Keep yourself in the middle half of the window.
    const long long du = cu - m.originU, dv = cv - m.originV;
    if (du < kMarkSize / 4 || du > 3 * kMarkSize / 4) {
        const long long shift = du - kMarkSize / 2;
        ClearBand(0, shift > 0 ? m.originU + kMarkSize : m.originU + shift, shift > 0 ? shift : -shift);
        m.originU += shift;
    }
    if (dv < kMarkSize / 4 || dv > 3 * kMarkSize / 4) {
        const long long shift = dv - kMarkSize / 2;
        ClearBand(1, shift > 0 ? m.originV + kMarkSize : m.originV + shift, shift > 0 ? shift : -shift);
        m.originV += shift;
    }
    // The grooves: from each wheel's last contact with the ground to this one.
    g_gl.Enable(GL_BLEND);
    g_gl.BlendEquationSeparate(kFuncMax, kFuncMax);
    g_gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    for (int i = 0; i < 2; ++i) {
        Wheel& w = g_wheels[i];
        if (!w.on || g_air.airborne) {
            w.haveLast = false;
            continue;
        }
        const Vec3 contact = w.contact;
        const double tu = Dot(contact, u) / kMarkTexel, tv = Dot(contact, v) / kMarkTexel;
        if (w.haveLast) {
            const double step = std::hypot(tu - w.lastU, tv - w.lastV) * kMarkTexel;
            if (step > 2.5) {  // a respawn: don't join it up
                w.haveLast = false;
            } else if (step < 0.01) {
                continue;  // standing still: keep the last contact
            } else {
                // Rear: a 240 mm rut (the tire digs and throws dirt aside), front: 180 mm, shallower.
                Groove(w.lastU, w.lastV, tu, tv, (i == 0 ? 0.12 : 0.09) / kMarkTexel, i == 0 ? 1.0 : 0.7);
            }
        }
        w.lastU = tu;
        w.lastV = tv;
        w.haveLast = true;
    }
    g_gl.BlendEquationSeparate(kFuncAdd, kFuncAdd);
    g_gl.Disable(GL_BLEND);
    g_gl.Disable(GL_SCISSOR_TEST);
    return true;
}

// World position of the front plate, the bike's forward and its up: always on your bike when
// it can be found (first person: the bike tracker; third person: the object under your rider),
// else on your rider, else on the game's camera.
bool HeadlightPose(Vec3& position, Vec3& forward, Vec3& up)
{
    const Config& cfg = GetConfig();
    double invRaw[16];
    if (!AffineInverse(g_frame.raw, invRaw))
        return false;
    const Vec3 worldUp = WorldUp();
    if (g_bikeNow.found) {
        PoseFromBike(g_bikeNow.model, position, forward, up);
        NoteHeadlightMode(g_bikeNow.how, position);
        return true;
    }
    if (g_rider.seen) {
        // Ahead of the middle of the rider and bike, at about the height of the plate.
        Vec3 flat = g_rider.haveHeading ? g_rider.heading : EyeToWorld(g_frame.raw, Vec3{ 0, 0, -1 });
        flat = flat - worldUp * Dot(flat, worldUp);
        if (Length(flat) < 1e-3)
            return false;
        forward = Normalize(flat);
        up = worldUp;
        position = g_rider.position + forward * 0.75 + worldUp * 0.05;
        NoteHeadlightMode(2, position);
        return true;
    }
    // Level with the ground, under the camera: the way the bike faces, near enough.
    Vec3 look = EyeToWorld(g_frame.raw, Vec3{ 0, 0, -1 });
    look = look - worldUp * Dot(look, worldUp);
    if (Length(look) < 1e-3)
        return false;
    forward = Normalize(look);
    up = worldUp;
    position = CameraPosition(g_frame.raw) + forward * cfg.headlightForward + up * cfg.headlightHeight;
    NoteHeadlightMode(3, position);
    return true;
}

// ===========================================================================
// The passes
// ===========================================================================
void Render(int w, int h)
{
    const Config& cfg = GetConfig();
    const double now = Seconds();
    const double dt = g_lastApplyTime > 0 ? Clamp(now - g_lastApplyTime, 1.0 / 1000.0, 0.25) : 1.0 / 60.0;
    g_lastApplyTime = now;
    g_lastDt = dt;

    g_gl.BindVertexArray(g_res.vao);  // the fullscreen triangle (the game's arrays are restored after)
    FindOwnRider(dt);
    UpdateBike(dt);
    UpdateWheels(now, dt);
    UpdateAir(dt);
    const bool marksOn = UpdateMarks();
    if (!CaptureColor())
        return;

    // ---- What's known about the scene ----
    const float* proj = g_haveProjCamera ? g_projCamera : (g_haveProjOther ? g_projOther : nullptr);
    double P[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }, invP[16];
    if (proj) {
        for (int i = 0; i < 16; ++i)
            P[i] = proj[i];
    } else if (!g_loggedNoProjection) {
        g_loggedNoProjection = true;
        Log("postfx: no perspective projection seen yet; depth-based effects wait for one");
    }
    const bool haveInverse = Invert(P, invP);
    const bool depthOk = proj && haveInverse && CaptureDepth(g_res.captureFbo, g_res.depth);
    g_lastDepthOk = depthOk;
    ToFloat(P, g_m.proj, 16);
    ToFloat(haveInverse ? invP : P, g_m.invProj, 16);
    g_m.texel[0] = 1.0f / w;
    g_m.texel[1] = 1.0f / h;

    const double* V = g_frame.rendered;
    if (g_frame.haveLight) {
        Vec3 sun;
        if (SunFromEye(g_frame.raw, g_frame.lightEye, sun)) {
            g_gameSun = sun;
            g_haveGameSun = true;
        }
    } else if (!g_haveGameSun && !g_loggedNoLight) {
        g_loggedNoLight = true;
        Log("postfx: no '%s' upload seen yet; sun shadows and sun rays wait for it", cfg.lightUniform);
    }
    const timeofday::Sky sky = CurrentSky();
    const bool tod = cfg.timeOfDay >= 0;
    // What lights the scene: the game's sun, or ours when the time of day turns it.
    const Vec3 lightWorld = SceneLight(sky);
    const Vec3 visibleSun = tod ? sky.sun : g_gameSun;
    const Vec3 lightView = Normalize(WorldToEye(V, lightWorld));
    const Vec3 up = WorldUp();

    float viewToWorld[9];
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            viewToWorld[c * 3 + r] = static_cast<float>(V[r * 4 + c]);
    const Vec3 eye = CameraPosition(V);
    const auto Wrap = [](double v) { const double m = std::fmod(v, 64.0); return m < 0 ? m + 64.0 : m; };
    const Vec3 camWrapped{ Wrap(eye.x), Wrap(eye.y), Wrap(eye.z) };

    // ---- Lighting: AO and sun shadows (half resolution) ----
    const double sunStrength = tod ? std::fmax(sky.day, 0.4 * sky.night) : 1.0;
    const double shadowAmount = (g_haveGameSun || tod) ? cfg.shadows * sunStrength : 0.0;
    const bool lightOn = depthOk && (cfg.ambientOcclusion > 0.0 || shadowAmount > 0.0);
    if (lightOn) {
        Program& p = Use(kLighting);
        Bind(p, "u_depth", 0, g_res.depth);
        Vec(p, "u_sunView", lightView);
        g_gl.Uniform1f(p["u_aoOn"], cfg.ambientOcclusion > 0.0 ? 1.0f : 0.0f);
        g_gl.Uniform1f(p["u_aoRadius"], static_cast<float>(cfg.aoRadius));
        g_gl.Uniform1f(p["u_shadowOn"], shadowAmount > 0.0 ? 1.0f : 0.0f);
        g_gl.Uniform1f(p["u_shadowLength"], static_cast<float>(cfg.shadowLength));
        Draw(g_res.light);
    }

    // ---- Composite -> linear HDR ----
    {
        Program& p = Use(kComposite);
        Bind(p, "u_color", 0, g_res.color);
        Bind(p, "u_depth", 1, g_res.depth);
        Bind(p, "u_terrainDepth", 2, g_res.terrainDepth);
        Bind(p, "u_light", 3, g_res.light.tex);
        Bind(p, "u_detail", 4, g_res.detail);
        // Always on its own unit: a shadow sampler may not share one with an ordinary sampler.
        Bind(p, "u_shadowMap", 5, g_res.shadowMap);
        const bool shadowMapOn = depthOk && g_shadow.usable && g_shadow.drawn && g_res.shadowMap && shadowAmount > 0.0;
        g_gl.Uniform1f(p["u_shadowMapOn"], shadowMapOn ? 1.0f : 0.0f);
        if (shadowMapOn) {
            double projection[16], viewToShadow[16];
            for (int i = 0; i < 16; ++i)
                projection[i] = g_shadow.projection[i];
            MatMul(projection, g_shadow.toLight, viewToShadow);
            float matrix[16];
            ToFloat(viewToShadow, matrix, 16);
            g_gl.UniformMatrix4fv(p["u_viewToShadow"], 1, GL_FALSE, matrix);
            g_gl.Uniform1f(p["u_shadowTexel"], 1.0f / g_res.shadowSize);
            // About 4 cm along the light, plus a little more for the bigger texels of a wider map.
            g_gl.Uniform1f(p["u_shadowBias"], static_cast<float>((0.04 + g_shadow.half * 0.002) / (2.0 * kShadowDepth)));
        }
        Vec(p, "u_lightView", lightView);
        // The headlight: only when the time of day makes it dark enough to see.
        const double headlight = tod && depthOk ? cfg.headlight * Clamp(1.1 - sky.day, 0.0, 1.0) : 0.0;
        Vec3 headPosition, headForward, headUp;
        const bool headlightOn = headlight > 0.0 && HeadlightPose(headPosition, headForward, headUp);
        g_gl.Uniform1f(p["u_headlight"], headlightOn ? static_cast<float>(headlight) : 0.0f);
        if (headlightOn) {
            const Vec3 forwardEye = Normalize(WorldToEye(V, headForward));
            const Vec3 upEye = Normalize(WorldToEye(V, headUp));
            Vec(p, "u_headPos", TransformPoint(V, headPosition));
            Vec(p, "u_headDir", forwardEye);
            Vec(p, "u_headRight", Normalize(Cross(forwardEye, upEye)));
            Vec(p, "u_headUp", upEye);
            // The beam: a round cone seen from the plate, like a real headlight's, from its bottom
            // edge on the ground just in front of the front wheel (65 degrees down) up to its top
            // edge, HeadlightPitchDeg - 6 degrees below level (0 by default: out to the distance).
            // Beam width spreads it sideways only.
            constexpr double kBottomDeg = 65.0;
            const double topDeg = Clamp(cfg.headlightPitchDeg - 6.0, -10.0, 40.0);
            g_gl.Uniform3f(p["u_headBeam"], static_cast<float>(cfg.headlightBeamDeg * kDegToRad),
                           static_cast<float>((kBottomDeg + topDeg) * 0.5 * kDegToRad),
                           static_cast<float>((kBottomDeg - topDeg) * 0.5 * kDegToRad));
        }
        // Tire marks: where the map's window is, from the camera, in the map's texels.
        Bind(p, "u_markMap", 6, marksOn ? g_res.marks.tex : 0);
        g_gl.Uniform1f(p["u_marks"], marksOn ? static_cast<float>(cfg.tireMarks) : 0.0f);
        if (marksOn) {
            Vec3 mu, mv;
            PlaneAxes(mu, mv);
            const Vec3 camera = CameraPosition(V);
            g_gl.Uniform2f(p["u_markCam"], static_cast<float>(Dot(camera, mu) / kMarkTexel - static_cast<double>(g_markWindow.originU)),
                           static_cast<float>(Dot(camera, mv) / kMarkTexel - static_cast<double>(g_markWindow.originV)));
            g_gl.Uniform2f(p["u_markPhys"], static_cast<float>(MarkWrap(g_markWindow.originU)),
                           static_cast<float>(MarkWrap(g_markWindow.originV)));
            Vec(p, "u_markU", WorldToEye(V, mu));
            Vec(p, "u_markV", WorldToEye(V, mv));
            Vec(p, "u_markUW", mu);
            Vec(p, "u_markVW", mv);
            g_gl.Uniform1f(p["u_markTexel"], static_cast<float>(kMarkTexel));
            g_gl.Uniform1f(p["u_markSize"], static_cast<float>(kMarkSize));
        }
        // Tire dirt: each wheel's own space (its axle along x, turning with it) from the camera's.
        g_gl.Uniform1f(p["u_tireDirt"], static_cast<float>(cfg.tireDirt));
        for (int i = 0; i < 2; ++i) {
            const Wheel& wheel = g_wheels[i];
            const bool on = depthOk && cfg.tireDirt > 0.0 && wheel.on;
            char name[32];
            snprintf(name, sizeof name, "u_wheelOn[%d]", i);
            g_gl.Uniform1f(p[name], on ? 1.0f : 0.0f);
            if (!on)
                continue;
            const Vec3 x = Normalize(WorldToEye(V, wheel.axes[wheel.axleAxis]));
            Vec3 y = WorldToEye(V, wheel.axes[(wheel.axleAxis + 1) % 3]);
            y = Normalize(y - x * Dot(y, x));
            const Vec3 z = Normalize(Cross(x, y)), o = TransformPoint(V, wheel.axle);
            // eye -> wheel: the transpose of the axes, and the origin moved there.
            const float local[16] = { static_cast<float>(x.x), static_cast<float>(y.x), static_cast<float>(z.x), 0.0f,
                                      static_cast<float>(x.y), static_cast<float>(y.y), static_cast<float>(z.y), 0.0f,
                                      static_cast<float>(x.z), static_cast<float>(y.z), static_cast<float>(z.z), 0.0f,
                                      static_cast<float>(-Dot(x, o)), static_cast<float>(-Dot(y, o)),
                                      static_cast<float>(-Dot(z, o)), 1.0f };
            snprintf(name, sizeof name, "u_wheelLocal[%d]", i);
            g_gl.UniformMatrix4fv(p[name], 1, GL_FALSE, local);
            snprintf(name, sizeof name, "u_wheelRadius[%d]", i);
            g_gl.Uniform1f(p[name], static_cast<float>(wheel.radius));
        }
        g_gl.Uniform2f(p["u_halfTexel"], 1.0f / g_res.light.w, 1.0f / g_res.light.h);
        g_gl.Uniform1f(p["u_depthOk"], depthOk ? 1.0f : 0.0f);
        g_gl.Uniform1f(p["u_lightOn"], lightOn ? 1.0f : 0.0f);
        g_gl.Uniform1f(p["u_ao"], static_cast<float>(cfg.ambientOcclusion));
        g_gl.Uniform1f(p["u_shadow"], static_cast<float>(shadowAmount));
        g_gl.Uniform1f(p["u_dirt"], static_cast<float>(cfg.dirtDetail));
        g_gl.Uniform1f(p["u_dirtScale"], static_cast<float>(cfg.dirtScale));
        g_gl.Uniform1f(p["u_haveTerrain"], g_frame.terrainSnapshot ? 1.0f : 0.0f);
        g_gl.UniformMatrix3fv(p["u_viewToWorld"], 1, GL_FALSE, viewToWorld);
        Vec(p, "u_camWrapped", camWrapped);
        Vec(p, "u_up", up);
        Vec(p, "u_lightWorld", lightWorld);
        Vec(p, "u_sunWorld", visibleSun);
        Vec(p, "u_moonWorld", sky.moon);
        g_gl.Uniform1f(p["u_tod"], tod ? 1.0f : 0.0f);
        Vec(p, "u_tint", sky.tint);
        g_gl.Uniform1f(p["u_exposure"], static_cast<float>(sky.exposure));
        g_gl.Uniform1f(p["u_desaturate"], static_cast<float>(sky.desaturate));
        g_gl.Uniform1f(p["u_day"], static_cast<float>(sky.day));
        g_gl.Uniform1f(p["u_golden"], static_cast<float>(sky.golden));
        g_gl.Uniform1f(p["u_night"], static_cast<float>(sky.night));
        g_gl.Uniform1f(p["u_haze"], static_cast<float>(sky.haze));
        g_gl.Uniform1f(p["u_nightLevel"], static_cast<float>(sky.nightLevel));
        g_gl.Uniform1f(p["u_time"], static_cast<float>(std::fmod(now, 1000.0)));
        const double fovY = 2.0 * std::atan(1.0 / std::fmax(std::fabs(P[5]), 1e-3));
        g_gl.Uniform1f(p["u_pixelAngle"], static_cast<float>(fovY / h));
        Draw(g_res.hdr);
    }
    g_lastTerrainMask = g_frame.terrainSnapshot;
    const Target* current = &g_res.hdr;
    const auto Spare = [&current]() -> Target& { return current == &g_res.a ? g_res.b : g_res.a; };

    // ---- Bloom ----
    const bool bloomOn = cfg.bloom > 0.0;
    if (bloomOn) {
        for (int i = 0; i < kBloomLevels; ++i) {
            const Target& src = i == 0 ? g_res.hdr : g_res.bloom[i - 1];
            Program& p = Use(kBloomDown);
            Bind(p, "u_src", 0, src.tex);
            g_gl.Uniform2f(p["u_srcTexel"], 1.0f / src.w, 1.0f / src.h);
            g_gl.Uniform1f(p["u_prefilter"], i == 0 ? 1.0f : 0.0f);
            g_gl.Uniform1f(p["u_threshold"], static_cast<float>(cfg.bloomThreshold));
            Draw(g_res.bloom[i]);
        }
        g_gl.Enable(GL_BLEND);
        g_gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        g_gl.BlendEquationSeparate(kFuncAdd, kFuncAdd);
        for (int i = kBloomLevels - 1; i > 0; --i) {
            Program& p = Use(kBloomUp);
            Bind(p, "u_src", 0, g_res.bloom[i].tex);
            g_gl.Uniform2f(p["u_srcTexel"], 1.0f / g_res.bloom[i].w, 1.0f / g_res.bloom[i].h);
            Draw(g_res.bloom[i - 1]);
        }
        g_gl.Disable(GL_BLEND);
    }

    // ---- Sun rays ----
    double raysAmount = 0;
    {
        const Vec3 sunView = WorldToEye(V, visibleSun);
        const double cx = P[0] * sunView.x + P[4] * sunView.y + P[8] * sunView.z;
        const double cy = P[1] * sunView.x + P[5] * sunView.y + P[9] * sunView.z;
        const double cw = P[3] * sunView.x + P[7] * sunView.y + P[11] * sunView.z;
        if (cfg.sunRays > 0.0 && depthOk && cw > 1e-4 && (g_haveGameSun || tod)) {
            const double u = cx / cw * 0.5 + 0.5, v = cy / cw * 0.5 + 0.5;
            const double off = std::fmax(std::fmax(-u, u - 1.0), std::fmax(-v, v - 1.0));  // > 0: off screen
            const double visible = 1.0 - Clamp(off / 0.4, 0.0, 1.0);
            const double daylight = tod ? Clamp((sky.sunElevationDeg + 2.0) / 6.0, 0.0, 1.0) : 1.0;
            raysAmount = cfg.sunRays * visible * daylight;
            if (raysAmount > 0.0) {
                Program& p = Use(kRays);
                Bind(p, "u_src", 0, g_res.hdr.tex);
                Bind(p, "u_depth", 1, g_res.depth);
                g_gl.Uniform2f(p["u_sunUV"], static_cast<float>(u), static_cast<float>(v));
                g_gl.Uniform1f(p["u_aspect"], static_cast<float>(w) / h);
                Draw(g_res.rays);
            }
        }
    }

    // ---- Depth of field ----
    if (cfg.depthOfField > 0.0 && depthOk) {
        const float maxBlur = static_cast<float>(12.0 * h / 1080.0);
        {
            Target& next = g_res.focus[1 - g_res.focusIndex];
            Program& p = Use(kFocus);
            Bind(p, "u_depth", 0, g_res.depth);
            Bind(p, "u_prev", 1, g_res.focus[g_res.focusIndex].tex);
            g_gl.Uniform1f(p["u_rate"], g_res.focusValid ? static_cast<float>(1.0 - std::exp(-dt / 0.3)) : 1.0f);
            g_gl.Uniform1f(p["u_manual"], static_cast<float>(cfg.dofFocus));
            Draw(next);
            g_res.focusIndex = 1 - g_res.focusIndex;
            g_res.focusValid = true;
        }
        const GLuint focus = g_res.focus[g_res.focusIndex].tex;
        {
            Program& p = Use(kDof);
            Bind(p, "u_src", 0, current->tex);
            Bind(p, "u_depth", 1, g_res.depth);
            Bind(p, "u_focus", 2, focus);
            g_gl.Uniform1f(p["u_strength"], static_cast<float>(cfg.depthOfField));
            g_gl.Uniform1f(p["u_maxBlur"], maxBlur);
            g_gl.Uniform1f(p["u_radScale"], maxBlur * maxBlur / 150.0f);
            g_gl.Uniform2f(p["u_halfTexel"], 1.0f / g_res.dof.w, 1.0f / g_res.dof.h);
            Draw(g_res.dof);
        }
        {
            Target& out = Spare();
            Program& p = Use(kDofMerge);
            Bind(p, "u_src", 0, current->tex);
            Bind(p, "u_blur", 1, g_res.dof.tex);
            Bind(p, "u_depth", 2, g_res.depth);
            Bind(p, "u_focus", 3, focus);
            g_gl.Uniform1f(p["u_strength"], static_cast<float>(cfg.depthOfField));
            g_gl.Uniform1f(p["u_maxBlur"], maxBlur);
            Draw(out);
            current = &out;
        }
    } else {
        g_res.focusValid = false;
    }

    // ---- Motion blur ----
    double viewProj[16];
    MatMul(P, V, viewProj);
    const Vec3 forward = Normalize(EyeToWorld(V, Vec3{ 0, 0, -1 }));
    if (cfg.motionBlur > 0.0 && depthOk && g_motion.valid) {
        const double moved = Length(eye - g_motion.eye);
        const double turned = std::acos(Clamp(Dot(forward, g_motion.forward), -1.0, 1.0)) * kRadToDeg;
        double invViewProj[16], reproj[16];
        if (moved < 4.0 && turned < 30.0 && Invert(viewProj, invViewProj)) {
            MatMul(g_motion.viewProj, invViewProj, reproj);
            float reprojF[16];
            ToFloat(reproj, reprojF, 16);
            Target& out = Spare();
            Program& p = Use(kMotion);
            Bind(p, "u_src", 0, current->tex);
            Bind(p, "u_depth", 1, g_res.depth);
            g_gl.UniformMatrix4fv(p["u_reproj"], 1, GL_FALSE, reprojF);
            // As if the shutter were open for this fraction of a 60 fps frame, at any frame rate.
            g_gl.Uniform1f(p["u_scale"], static_cast<float>(cfg.motionBlur * Clamp((1.0 / 60.0) / dt, 0.25, 4.0)));
            g_gl.Uniform1f(p["u_maxPx"], static_cast<float>(48.0 * h / 1080.0));
            Draw(out);
            current = &out;
        }
    }
    for (int i = 0; i < 16; ++i)
        g_motion.viewProj[i] = viewProj[i];
    g_motion.eye = eye;
    g_motion.forward = forward;
    g_motion.valid = true;

    // ---- Grade -> display colours ----
    {
        Program& p = Use(kGrade);
        Bind(p, "u_src", 0, current->tex);
        Bind(p, "u_bloom", 1, bloomOn ? g_res.bloom[0].tex : 0);
        Bind(p, "u_rays", 2, raysAmount > 0.0 ? g_res.rays.tex : 0);
        g_gl.Uniform1f(p["u_bloomAmount"], bloomOn ? static_cast<float>(cfg.bloom * 0.12) : 0.0f);
        g_gl.Uniform1f(p["u_raysAmount"], static_cast<float>(raysAmount));
        const Vec3 raysTint = tod ? (Vec3{ 1.0, 0.95, 0.85 } + (Vec3{ 1.0, 0.62, 0.35 } - Vec3{ 1.0, 0.95, 0.85 }) * sky.golden)
                                  : Vec3{ 1.0, 0.95, 0.85 };
        Vec(p, "u_raysTint", raysTint);
        g_gl.Uniform1f(p["u_exposure"], static_cast<float>(std::pow(2.0, cfg.exposure)));
        g_gl.Uniform1f(p["u_contrast"], static_cast<float>(cfg.contrast));
        g_gl.Uniform1f(p["u_saturation"], static_cast<float>(cfg.saturation));
        g_gl.Uniform1f(p["u_vignette"], static_cast<float>(cfg.vignette));
        g_gl.Uniform1f(p["u_aspect"], static_cast<float>(w) / h);
        Draw(g_res.ldr);
    }

    // ---- FXAA + sharpening -> the window ----
    {
        static const float kFastSteps[12]    = { 1.0f, 1.5f, 2.0f, 4.0f, 12.0f };
        static const float kQualitySteps[12] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.5f, 2.0f, 2.0f, 2.0f, 2.0f, 4.0f, 8.0f };
        const bool quality = cfg.antiAliasing >= 2;
        Program& p = Use(kFinal);
        Bind(p, "u_src", 0, g_res.ldr.tex);
        g_gl.Uniform1f(p["u_fxaa"], cfg.antiAliasing > 0 ? 1.0f : 0.0f);
        g_gl.Uniform1fv(p["u_steps"], 12, quality ? kQualitySteps : kFastSteps);
        g_gl.Uniform1i(p["u_stepCount"], quality ? 12 : 5);
        g_gl.Uniform1f(p["u_edgeThreshold"], quality ? 0.125f : 0.166f);
        g_gl.Uniform1f(p["u_edgeThresholdMin"], quality ? 0.0625f : 0.0833f);
        g_gl.Uniform1f(p["u_subpix"], 0.75f);
        g_gl.Uniform1f(p["u_sharpen"], static_cast<float>(cfg.sharpen));
        Draw(nullptr, w, h);
    }
}

bool WindowSize(int& w, int& h)
{
    const HDC dc = g_gl.GetCurrentDC();
    const HWND window = dc ? WindowFromDC(dc) : nullptr;
    RECT client;
    if (!window || !GetClientRect(window, &client))
        return false;
    w = client.right;
    h = client.bottom;
    return w > 0 && h > 0;
}

void Apply()
{
    if (!LoadGL())
        return;
    int w = 0, h = 0;
    if (!WindowSize(w, h))
        return;
    SavedState saved;
    Save(saved);
    PrepareState();
    if (EnsureResources(w, h)) {
        if (!g_loggedFirstApply) {
            g_loggedFirstApply = true;
            Log("postfx: running at %dx%d", w, h);
        }
        Render(w, h);
    }
    Restore(saved);
}

// The context's resources exist (made with the game's state saved around it, if not).
bool ReadyResources()
{
    int w = 0, h = 0;
    if (!WindowSize(w, h))
        return false;
    if (g_res.context == g_gl.GetCurrentContext() && g_res.programs[0].id && w == g_res.width && h == g_res.height)
        return !g_res.failed;
    SavedState saved;
    Save(saved);
    PrepareState();
    const bool ok = EnsureResources(w, h);
    Restore(saved);
    return ok;
}

// The shadow map's depth texture and framebuffer, at the configured size.
bool EnsureShadowMap()
{
    const int size = GetConfig().shadowMapSize;
    if (g_res.shadowMap && g_res.shadowSize == size)
        return true;
    GLint active = 0, bound = 0;
    g_gl.GetIntegerv(kActiveTexture, &active);
    g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    GLint unpackBuffer = 0;
    g_gl.GetIntegerv(kPixelUnpackBufferBinding, &unpackBuffer);
    g_gl.BindBuffer(kPixelUnpackBuffer, 0);
    if (g_res.shadowMap)
        g_gl.DeleteTextures(1, &g_res.shadowMap);
    g_res.shadowMap = NewTexture(kDepthComponent24, size, size, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, kTextureCompareMode, kCompareRefToTexture);  // hardware PCF
    g_gl.TexParameteri(GL_TEXTURE_2D, kTextureCompareFunc, GL_LEQUAL);
    g_gl.BindTexture(GL_TEXTURE_2D, bound);
    g_gl.ActiveTexture(active);
    g_gl.BindBuffer(kPixelUnpackBuffer, unpackBuffer);
    if (!g_res.shadowFbo)
        g_gl.GenFramebuffers(1, &g_res.shadowFbo);
    g_gl.BindFramebuffer(kDrawFramebuffer, g_res.shadowFbo);
    g_gl.FramebufferTexture2D(kDrawFramebuffer, kDepthAttachment, GL_TEXTURE_2D, g_res.shadowMap, 0);
    g_gl.DrawBuffer(GL_NONE);
    const bool ok = g_gl.CheckFramebufferStatus(kDrawFramebuffer) == kFramebufferComplete;
    g_res.shadowSize = size;
    Log("postfx: shadow map %dx%d %s", size, size, ok ? "ready" : "could not be made");
    if (!ok) {
        g_gl.DeleteTextures(1, &g_res.shadowMap);
        g_res.shadowMap = 0;
    }
    return ok;
}

// The game's state a shadow draw changes.
struct ShadowSaved {
    GLint     drawFb = 0, viewport[4] = {}, depthFunc = GL_LESS;
    GLfloat   depthRange[2] = { 0, 1 };
    GLboolean depthMask = GL_TRUE;
    bool      depthTest = false, scissorTest = false, stencilTest = false;
};
ShadowSaved g_shadowSaved;
bool g_loggedShadowCasters = false;

}  // namespace

// ===========================================================================
// Interface
// ===========================================================================
bool WantsShadowCasters()
{
    const Config& cfg = GetConfig();
    return cfg.shaders && cfg.shadowMap && cfg.shadows > 0.0 && g_frame.haveView && !g_frame.applied && !g_res.failed &&
           g_glState >= 0 && (!g_shadow.setup || g_shadow.usable);
}

bool ShadowMatrices(const float uploaded[16], const float* anchorEye, float lightModelview[16], float lightProjection[16])
{
    if (!g_shadow.setup)
        SetupShadowFrame();
    if (!g_shadow.usable)
        return false;
    double modelview[16], toLight[16];
    for (int i = 0; i < 16; ++i)
        modelview[i] = uploaded[i];
    MatMul(g_shadow.toLight, modelview, toLight);

    // Only what's near enough to throw a shadow inside the map. A mesh placed in world space (its
    // model matrix is the identity) can't be judged by its origin: it is always drawn.
    Vec3 at;
    if (anchorEye) {
        at = TransformPoint(g_shadow.toLight, Vec3{ anchorEye[0], anchorEye[1], anchorEye[2] });
    } else {
        double invView[16], model[16];
        bool worldSpace = false;
        if (AffineInverse(g_frame.rendered, invView)) {
            MatMul(invView, modelview, model);
            worldSpace = std::fabs(model[12]) + std::fabs(model[13]) + std::fabs(model[14]) < 1e-2 &&
                         std::fabs(model[0] - 1) + std::fabs(model[5] - 1) + std::fabs(model[10] - 1) < 1e-3;
        }
        at = worldSpace ? Vec3{} : Vec3{ toLight[12], toLight[13], toLight[14] };
    }
    const double reach = g_shadow.half * 1.5;
    if (std::fabs(at.x) > reach || std::fabs(at.y) > reach || std::fabs(at.z) > kShadowDepth)
        return false;
    ToFloat(toLight, lightModelview, 16);
    for (int i = 0; i < 16; ++i)
        lightProjection[i] = g_shadow.projection[i];
    return true;
}

bool BeginShadowDraw()
{
    if (!g_shadow.usable || !LoadGL())
        return false;
    GLint drawFb = 0;
    g_gl.GetIntegerv(kDrawFramebufferBinding, &drawFb);
    if (drawFb != 0 || !ReadyResources())
        return false;  // the game is drawing into a texture of its own, not the scene

    ShadowSaved& s = g_shadowSaved;
    s.drawFb = drawFb;
    g_gl.GetIntegerv(GL_VIEWPORT, s.viewport);
    g_gl.GetIntegerv(GL_DEPTH_FUNC, &s.depthFunc);
    g_gl.GetFloatv(GL_DEPTH_RANGE, s.depthRange);
    g_gl.GetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
    s.depthTest   = g_gl.IsEnabled(GL_DEPTH_TEST) != GL_FALSE;
    s.scissorTest = g_gl.IsEnabled(GL_SCISSOR_TEST) != GL_FALSE;
    s.stencilTest = g_gl.IsEnabled(GL_STENCIL_TEST) != GL_FALSE;

    if (!EnsureShadowMap()) {
        g_gl.BindFramebuffer(kDrawFramebuffer, s.drawFb);
        g_shadow.usable = false;
        return false;
    }
    g_gl.BindFramebuffer(kDrawFramebuffer, g_res.shadowFbo);
    g_gl.Viewport(0, 0, g_res.shadowSize, g_res.shadowSize);
    g_gl.Enable(GL_DEPTH_TEST);
    g_gl.DepthFunc(GL_LESS);
    g_gl.DepthMask(GL_TRUE);
    g_gl.DepthRange(0.0, 1.0);
    g_gl.Disable(GL_SCISSOR_TEST);
    g_gl.Disable(GL_STENCIL_TEST);
    if (!g_shadow.cleared) {
        GLfloat clearDepth = 1.0f;
        g_gl.GetFloatv(GL_DEPTH_CLEAR_VALUE, &clearDepth);
        g_gl.ClearDepth(1.0);
        g_gl.Clear(GL_DEPTH_BUFFER_BIT);
        g_gl.ClearDepth(clearDepth);
        g_shadow.cleared = true;
        if (!g_loggedShadowCasters) {
            g_loggedShadowCasters = true;
            Log("postfx: objects cast shadow-map shadows (%.0f m around the camera)", g_shadow.half);
        }
    }
    g_shadow.drawn = true;
    return true;
}

void EndShadowDraw()
{
    const ShadowSaved& s = g_shadowSaved;
    g_gl.BindFramebuffer(kDrawFramebuffer, s.drawFb);
    g_gl.Viewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    g_gl.DepthFunc(s.depthFunc);
    g_gl.DepthMask(s.depthMask);
    g_gl.DepthRange(s.depthRange[0], s.depthRange[1]);
    SetCapability(GL_DEPTH_TEST, s.depthTest);
    SetCapability(GL_SCISSOR_TEST, s.scissorTest);
    SetCapability(GL_STENCIL_TEST, s.stencilTest);
}
void NoteView(const double raw[16], const double rendered[16])
{
    if (g_frame.haveView)
        return;
    g_frame.haveView = true;
    for (int i = 0; i < 16; ++i) {
        g_frame.raw[i] = raw[i];
        g_frame.rendered[i] = rendered[i];
    }
}

void NoteProjection(const float m[16], bool fromCamera)
{
    float* slot = fromCamera ? g_projCamera : g_projOther;
    for (int i = 0; i < 16; ++i)
        slot[i] = m[i];
    (fromCamera ? g_haveProjCamera : g_haveProjOther) = true;
}

void NoteGameLight(const float* value, int components)
{
    if (!value || (components == 4 && value[3] != 0.0f))
        return;  // a point light, not the sun
    g_frame.haveLight = true;
    for (int i = 0; i < 3; ++i)
        g_frame.lightEye[i] = value[i];
    // The game lit this frame with this frame's view: only that gives the sun exactly (it is
    // worked out again from this upload once the frame's camera is known).
    Vec3 sun;
    if (const double* raw = g_frame.haveView || !g_haveGameSun ? BestRawView() : nullptr) {
        if (SunFromEye(raw, value, sun)) {
            g_gameSun = sun;
            g_haveGameSun = true;
        }
    }
}

bool OverrideLight(const float* value, int count, int components, float* out)
{
    const Config& cfg = GetConfig();
    const double* raw = BestRawView();
    if (!cfg.shaders || !cfg.moveSun || cfg.timeOfDay < 0 || !raw || !value || !g_haveGameSun)
        return false;
    const timeofday::Sky sky = timeofday::Compute(cfg.timeOfDay, g_gameSun, WorldUp(), cfg.nightBrightness);
    const Vec3 eye = Normalize(WorldToEye(raw, sky.light * (cfg.invertLight ? -1.0 : 1.0)));
    for (int i = 0; i < count; ++i) {
        const float* v = value + i * components;
        float* o = out + i * components;
        for (int k = 0; k < components; ++k)
            o[k] = v[k];
        if (components == 4 && v[3] != 0.0f)
            continue;  // a point light
        const double length = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
        o[0] = static_cast<float>(eye.x * length);
        o[1] = static_cast<float>(eye.y * length);
        o[2] = static_cast<float>(eye.z * length);
    }
    return true;
}

void TerrainPassDone()
{
    const Config& cfg = GetConfig();
    if (!cfg.shaders || (cfg.dirtDetail <= 0.0 && cfg.tireMarks <= 0.0 && cfg.tireDirt <= 0.0) || !g_frame.haveView || g_frame.terrainSnapshot || g_frame.applied ||
        g_res.failed || !LoadGL())
        return;
    GLint drawFb = 0;
    g_gl.GetIntegerv(kDrawFramebufferBinding, &drawFb);
    if (drawFb != 0) {
        if (!g_loggedTerrainFbo) {
            g_loggedTerrainFbo = true;
            Log("postfx: the terrain is drawn into framebuffer %d, not the window: no ground mask for the dirt detail", drawFb);
        }
        return;
    }
    int w = 0, h = 0;
    if (!WindowSize(w, h))
        return;
    SavedState saved;
    Save(saved);
    PrepareState();
    if (EnsureResources(w, h) && CaptureDepth(g_res.terrainFbo, g_res.terrainDepth))
        g_frame.terrainSnapshot = true;
    Restore(saved);
}

void NoteSkeleton(unsigned program, const float* centres, const float* positions, const float* rotations, bool transposed,
                  int bones, const float modelview[16])
{
    if (!centres || !positions || !rotations || !modelview || bones <= 0 || bones > kSkeletonBones ||
        g_skeletons.size() >= 400)
        return;
    // Only what's near the camera matters (your rider, your bike): skip the rest of the track.
    {
        const float* m = modelview;
        const float x = positions[0], y = positions[1], z = positions[2];
        const float ex = m[0] * x + m[4] * y + m[8] * z + m[12], ey = m[1] * x + m[5] * y + m[9] * z + m[13],
                    ez = m[2] * x + m[6] * y + m[10] * z + m[14];
        if (ex * ex + ey * ey + ez * ez > 60.0f * 60.0f)
            return;
    }
    // The same draw again (a rider takes several): skip it.
    for (const SkeletonRecord& rec : g_skeletons)
        if (rec.program == program && rec.bones == bones && std::memcmp(rec.viewModel, modelview, sizeof rec.viewModel) == 0 &&
            std::memcmp(rec.position, positions, sizeof(float) * 3 * bones) == 0)
            return;
    SkeletonRecord rec;
    rec.program = program;
    rec.bones = bones;
    rec.transposed = transposed;
    std::copy(modelview, modelview + 16, rec.viewModel);
    std::copy(positions, positions + bones * 3, rec.position);
    std::copy(rotations, rotations + bones * 9, rec.rotation);
    // A rider: bones off the body's midline (arms, legs) and the highest one on it, the torso.
    double halfSpan = 0;
    for (int b = 0; b < bones; ++b)
        halfSpan = std::fmax(halfSpan, std::fabs(double(centres[b * 3])));
    int torso = -1, midline = 0;
    for (int b = 0; b < bones; ++b) {
        if (std::fabs(centres[b * 3]) > 0.05 * halfSpan)
            continue;
        ++midline;
        if (torso < 0 || centres[b * 3 + 1] > centres[torso * 3 + 1])
            torso = b;
    }
    rec.rider = halfSpan > 0.05 && torso >= 0 && bones >= 8 && bones <= 12 && bones - midline >= 6;
    rec.torso = rec.rider ? torso : -1;
    g_skeletons.push_back(rec);
}

void NoteObject(unsigned program, const float viewModel[16])
{
    if (!viewModel || g_objects.size() >= 1024)
        return;
    const double distance = std::sqrt(double(viewModel[12]) * viewModel[12] + double(viewModel[13]) * viewModel[13] +
                                      double(viewModel[14]) * viewModel[14]);
    if (distance > 40.0)
        return;  // your bike is never this far from the camera (the far chase camera sits ~12 m back)
    SceneObject o;
    o.program = program;
    std::copy(viewModel, viewModel + 16, o.viewModel);
    g_objects.push_back(o);
}


bool HidesGameFlare()
{
    const Config& cfg = GetConfig();
    return cfg.shaders && cfg.hideGameFlare && cfg.timeOfDay >= 0 && (g_frame.haveView || g_lastFrame3D);
}

bool WantsSceneEnd()
{
    const Config& cfg = GetConfig();
    return cfg.shaders && cfg.applyBeforeHud && g_frame.haveView && !g_frame.applied && !g_res.failed && g_glState >= 0;
}

void SceneEnd(unsigned program, const char* why)
{
    if (!WantsSceneEnd() || !LoadGL())
        return;
    GLint drawFb = 0;
    g_gl.GetIntegerv(kDrawFramebufferBinding, &drawFb);
    if (drawFb != 0)
        return;  // drawing into a texture of the game's, not over the scene
    g_frame.applied = true;
    if (!g_loggedBeforeHud) {
        g_loggedBeforeHud = true;
        Log("postfx: applied before the HUD (first HUD draw: program %u, %s)", program, why);
    }
    DumpCall("^ POSTFX applied here: program %u is the start of the HUD (%s)", program, why);
    Apply();
}

void FrameEnd(bool apply)
{
    const Config& cfg = GetConfig();
    if (apply && cfg.shaders && g_frame.haveView && !g_frame.applied) {
        if (!g_loggedEndOfFrame) {
            g_loggedEndOfFrame = true;
            Log("postfx: applied at the end of the frame (%s)", cfg.applyBeforeHud ? "no HUD draw was recognised" : "ApplyBeforeHud=0");
        }
        g_frame.applied = true;
        Apply();
    }
    if (g_frame.haveView) {
        for (int i = 0; i < 16; ++i)
            g_lastRaw[i] = g_frame.raw[i];
        g_haveLastRaw = true;
    }
    g_lastFrame3D = g_frame.haveView;
    g_frame = Frame{};
    g_skeletons.clear();
    g_objects.clear();
    g_shadow = ShadowFrame{};
}

const char* StatusText()
{
    const Config& cfg = GetConfig();
    char keyName[32] = "";
    const UINT scanCode = MapVirtualKeyA(static_cast<UINT>(cfg.shaderToggleKey), MAPVK_VK_TO_VSC);
    if (!scanCode || GetKeyNameTextA(static_cast<LONG>(scanCode << 16), keyName, sizeof keyName) <= 0)
        snprintf(keyName, sizeof keyName, "KEY %02X", cfg.shaderToggleKey);
    for (char* c = keyName; *c; ++c)
        *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));

    if (!cfg.shaders)
        snprintf(g_status, sizeof g_status, "SHADERS OFF   \xB7   %s TOGGLES", keyName);
    else if (g_glState < 0 || g_res.failed)
        snprintf(g_status, sizeof g_status, "SHADERS UNAVAILABLE - SEE MXCAM.LOG");
    else if (g_lastApplyTime < 0 || Seconds() - g_lastApplyTime > 1.0)
        snprintf(g_status, sizeof g_status, "WAITING FOR THE 3D VIEW");
    else
        snprintf(g_status, sizeof g_status, "SHADERS ON%s%s%s   \xB7   %.0f FPS",
                 g_lastDepthOk ? "" : " - NO DEPTH",
                 g_lastDepthOk && cfg.dirtDetail > 0 && !g_lastTerrainMask ? " - NO GROUND" : "",
                 g_haveGameSun ? "" : " - NO SUN", 1.0 / g_lastDt);
    return g_status;
}

}  // namespace postfx
}  // namespace mxcam
