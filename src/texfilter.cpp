// texfilter.cpp
#include <windows.h>
#include <GL/gl.h>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "config.h"
#include "hooks.h"
#include "log.h"
#include "mathlib.h"
#include "texfilter.h"

namespace mxcam {
namespace texfilter {
namespace {

constexpr GLenum kMaxAnisotropy    = 0x84FE;  // GL_TEXTURE_MAX_ANISOTROPY(_EXT)
constexpr GLenum kMaxMaxAnisotropy = 0x84FF;  // GL_MAX_TEXTURE_MAX_ANISOTROPY(_EXT)
constexpr GLenum kLodBias          = 0x8501;  // GL_TEXTURE_LOD_BIAS

struct GLApi {
    decltype(&::glGetIntegerv)   GetIntegerv;
    decltype(&::glGetFloatv)     GetFloatv;
    decltype(&::glGetError)      GetError;
    decltype(&::glBindTexture)   BindTexture;
    decltype(&::glIsTexture)     IsTexture;
    decltype(&::glTexParameteri) TexParameteri;
    decltype(&::glTexParameterf) TexParameterf;
    GLboolean (APIENTRY* IsSampler)(GLuint);                         // optional (GL 3.3)
    void      (APIENTRY* SamplerParameteri)(GLuint, GLenum, GLint);
    void      (APIENTRY* SamplerParameterf)(GLuint, GLenum, GLfloat);
};
GLApi g_gl{};
int   g_glState = 0;          // 0 = not loaded, 1 = ready, -1 = unavailable
float g_maxAnisotropy = 0;    // 0 = anisotropic filtering not supported

// What the game itself asked for, per texture / sampler.
struct Params {
    float filter     = GL_NEAREST_MIPMAP_LINEAR;  // OpenGL's default minification filter
    float anisotropy = 1.0f;
    float lodBias    = 0.0f;
};

struct Settings {
    bool  trilinear  = false;
    float anisotropy = 1.0f;
    float lodBias    = 0.0f;
    bool operator==(const Settings& o) const
    {
        return trilinear == o.trilinear && anisotropy == o.anisotropy && lodBias == o.lodBias;
    }
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<GLuint, Params> g_textures, g_samplers;
Settings g_applied;
bool     g_appliedValid = false;
char     g_status[96] = "";

bool Load()
{
    if (g_glState != 0)
        return g_glState > 0;
    g_gl.GetIntegerv   = reinterpret_cast<decltype(g_gl.GetIntegerv)>(GetRealGLFunction("glGetIntegerv"));
    g_gl.GetFloatv     = reinterpret_cast<decltype(g_gl.GetFloatv)>(GetRealGLFunction("glGetFloatv"));
    g_gl.GetError      = reinterpret_cast<decltype(g_gl.GetError)>(GetRealGLFunction("glGetError"));
    g_gl.BindTexture   = reinterpret_cast<decltype(g_gl.BindTexture)>(GetRealGLFunction("glBindTexture"));
    g_gl.IsTexture     = reinterpret_cast<decltype(g_gl.IsTexture)>(GetRealGLFunction("glIsTexture"));
    g_gl.TexParameteri = reinterpret_cast<decltype(g_gl.TexParameteri)>(GetRealGLFunction("glTexParameteri"));
    g_gl.TexParameterf = reinterpret_cast<decltype(g_gl.TexParameterf)>(GetRealGLFunction("glTexParameterf"));
    g_gl.IsSampler         = reinterpret_cast<decltype(g_gl.IsSampler)>(GetRealGLFunction("glIsSampler"));
    g_gl.SamplerParameteri = reinterpret_cast<decltype(g_gl.SamplerParameteri)>(GetRealGLFunction("glSamplerParameteri"));
    g_gl.SamplerParameterf = reinterpret_cast<decltype(g_gl.SamplerParameterf)>(GetRealGLFunction("glSamplerParameterf"));
    if (!g_gl.GetIntegerv || !g_gl.GetFloatv || !g_gl.GetError || !g_gl.BindTexture || !g_gl.IsTexture ||
        !g_gl.TexParameteri || !g_gl.TexParameterf) {
        g_glState = -1;
        return false;
    }
    // Anisotropic filtering is an extension before OpenGL 4.6: asking for its limit tells.
    for (int i = 0; i < 32 && g_gl.GetError() != GL_NO_ERROR; ++i) {
    }
    GLfloat limit = 0;
    g_gl.GetFloatv(kMaxMaxAnisotropy, &limit);
    g_maxAnisotropy = g_gl.GetError() == GL_NO_ERROR && limit >= 1.0f ? limit : 0.0f;
    Log("texture filtering: %s", g_maxAnisotropy > 0 ? "anisotropic filtering available" : "no anisotropic filtering on this driver");
    if (g_maxAnisotropy > 0)
        Log("texture filtering: up to %gx anisotropy", g_maxAnisotropy);
    g_glState = 1;
    return true;
}

Settings Wanted()
{
    const Config& cfg = GetConfig();
    Settings s;
    if (!cfg.shaders)
        return s;
    s.trilinear  = cfg.trilinear;
    s.anisotropy = g_maxAnisotropy > 0 ? static_cast<float>(Clamp(cfg.anisotropy, 1.0, static_cast<double>(g_maxAnisotropy))) : 1.0f;
    s.lodBias    = static_cast<float>(cfg.textureLodBias);
    return s;
}

// Smooth (linear) filtering between texels with mipmaps: the textures that are ours to improve.
bool Smooth(float filter) { return filter == GL_LINEAR_MIPMAP_NEAREST || filter == GL_LINEAR_MIPMAP_LINEAR; }

float Filter(const Params& p, const Settings& s)
{
    return s.trilinear && p.filter == GL_LINEAR_MIPMAP_NEAREST ? static_cast<float>(GL_LINEAR_MIPMAP_LINEAR) : p.filter;
}
float Anisotropy(const Params& p, const Settings& s) { return Smooth(p.filter) ? std::fmax(p.anisotropy, s.anisotropy) : p.anisotropy; }
float LodBias(const Params& p, const Settings& s) { return Smooth(p.filter) ? p.lodBias + s.lodBias : p.lodBias; }

// Records the game's value, returns ours.
float Note(Params& p, unsigned pname, float value, const Settings& s)
{
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: p.filter = value;     return Filter(p, s);
    case kMaxAnisotropy:        p.anisotropy = value; return Anisotropy(p, s);
    case kLodBias:              p.lodBias = value;    return LodBias(p, s);
    default:                    return value;
    }
}

}  // namespace

bool Watches(unsigned pname)
{
    return pname == GL_TEXTURE_MIN_FILTER || pname == kMaxAnisotropy || pname == kLodBias;
}

float TextureParameter(unsigned target, unsigned pname, float value)
{
    if (target != GL_TEXTURE_2D || !Watches(pname) || !Load())
        return value;
    GLint bound = 0;
    g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    if (bound == 0)
        return value;
    const Settings s = Wanted();
    AcquireSRWLockExclusive(&g_lock);
    Params& p = g_textures[static_cast<GLuint>(bound)];
    const float result = Note(p, pname, value, s);
    const Params params = p;
    ReleaseSRWLockExclusive(&g_lock);
    // A new filter changes what the anisotropy and bias should be: they come along with it.
    if (pname == GL_TEXTURE_MIN_FILTER) {
        if (g_maxAnisotropy > 0)
            g_gl.TexParameterf(GL_TEXTURE_2D, kMaxAnisotropy, Anisotropy(params, s));
        g_gl.TexParameterf(GL_TEXTURE_2D, kLodBias, LodBias(params, s));
    }
    return result;
}

float SamplerParameter(unsigned sampler, unsigned pname, float value)
{
    if (!Watches(pname) || !Load() || !g_gl.SamplerParameterf)
        return value;
    const Settings s = Wanted();
    AcquireSRWLockExclusive(&g_lock);
    Params& p = g_samplers[sampler];
    const float result = Note(p, pname, value, s);
    const Params params = p;
    ReleaseSRWLockExclusive(&g_lock);
    if (pname == GL_TEXTURE_MIN_FILTER) {
        if (g_maxAnisotropy > 0)
            g_gl.SamplerParameterf(sampler, kMaxAnisotropy, Anisotropy(params, s));
        g_gl.SamplerParameterf(sampler, kLodBias, LodBias(params, s));
    }
    return result;
}

void TexturesDeleted(int count, const unsigned* textures)
{
    if (!textures)
        return;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < count; ++i)
        g_textures.erase(textures[i]);
    ReleaseSRWLockExclusive(&g_lock);
}

void SamplersDeleted(int count, const unsigned* samplers)
{
    if (!samplers)
        return;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < count; ++i)
        g_samplers.erase(samplers[i]);
    ReleaseSRWLockExclusive(&g_lock);
}

void FrameEnd()
{
    if (!Load())
        return;
    const Settings s = Wanted();
    if (g_appliedValid && s == g_applied)
        return;
    const bool first = !g_appliedValid;
    g_applied = s;
    g_appliedValid = true;

    GLint bound = 0;
    g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    int textures = 0, samplers = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (auto it = g_textures.begin(); it != g_textures.end();) {
        if (!g_gl.IsTexture(it->first)) {
            it = g_textures.erase(it);
            continue;
        }
        const Params& p = it->second;
        g_gl.BindTexture(GL_TEXTURE_2D, it->first);
        g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(Filter(p, s)));
        if (g_maxAnisotropy > 0)
            g_gl.TexParameterf(GL_TEXTURE_2D, kMaxAnisotropy, Anisotropy(p, s));
        g_gl.TexParameterf(GL_TEXTURE_2D, kLodBias, LodBias(p, s));
        textures += Smooth(p.filter) ? 1 : 0;
        ++it;
    }
    if (g_gl.SamplerParameterf && g_gl.SamplerParameteri && g_gl.IsSampler) {
        for (auto it = g_samplers.begin(); it != g_samplers.end();) {
            if (!g_gl.IsSampler(it->first)) {
                it = g_samplers.erase(it);
                continue;
            }
            const Params& p = it->second;
            g_gl.SamplerParameteri(it->first, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(Filter(p, s)));
            if (g_maxAnisotropy > 0)
                g_gl.SamplerParameterf(it->first, kMaxAnisotropy, Anisotropy(p, s));
            g_gl.SamplerParameterf(it->first, kLodBias, LodBias(p, s));
            samplers += Smooth(p.filter) ? 1 : 0;
            ++it;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    g_gl.BindTexture(GL_TEXTURE_2D, bound);
    if (!first || textures + samplers > 0)
        Log("texture filtering: %d texture(s), %d sampler(s) now at %gx anisotropy, %s, LOD bias %+.2f", textures,
            samplers, s.anisotropy, s.trilinear ? "trilinear" : "as the game set them", s.lodBias);
}

const char* StatusText()
{
    const Config& cfg = GetConfig();
    AcquireSRWLockShared(&g_lock);
    int smooth = 0;
    for (const auto& entry : g_textures)
        smooth += Smooth(entry.second.filter) ? 1 : 0;
    for (const auto& entry : g_samplers)
        smooth += Smooth(entry.second.filter) ? 1 : 0;
    ReleaseSRWLockShared(&g_lock);
    if (!cfg.shaders)
        snprintf(g_status, sizeof g_status, "SHADERS OFF: THE GAME'S OWN FILTERING");
    else if (g_glState == 0)
        snprintf(g_status, sizeof g_status, "TEXTURES: WAITING FOR THE GAME");
    else if (g_maxAnisotropy <= 0)
        snprintf(g_status, sizeof g_status, "NO ANISOTROPIC FILTERING   \xB7   %d TEXTURES", smooth);
    else
        snprintf(g_status, sizeof g_status, "%.0fX ANISOTROPIC   \xB7   %d TEXTURES", g_applied.anisotropy, smooth);
    return g_status;
}

}  // namespace texfilter
}  // namespace mxcam
