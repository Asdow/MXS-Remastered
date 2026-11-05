// postfx_test.cpp
//   End-to-end check of the shaders (postfx.cpp) and texture filtering (texfilter.cpp) on a real
//   GL context, through a built MXCam opengl32.dll. It draws a small scene the way MX Simulator
//   does - terrain patches from a program with u_heights / u_meshoffset / u_meshscale, a lit
//   object, then a scaled HUD element - and reads the frame back right after the HUD draw (the
//   effects run just before it).
//
//   The DLL is copied into a folder of its own (under %TEMP%) with an mxcam.ini written for the
//   mode, so the shipped settings are never touched:
//     off       Shaders=0: the picture must be exactly what was drawn
//     neutral   every effect at zero: the round trip through the pipeline must not change it
//     dirt      dirt detail only: the ground gets detail, the object and the sky don't
//     night     TimeOfDay=0: dark ground, a night sky, the HUD untouched, the sun turned
//     shadows   screen-space sun shadows only: the object shades the ground beside it
//     offscreen shadow maps: a tower behind the camera shades the ground in view
//     headlight TimeOfDay=0 and the headlight: a lit pool ahead, the rest night
//     tires     your bike rolling past on spinning wheels: a rut behind the rear one, mud on the tire
//     tireshidden  the same at 5 m/s for ~3.5 s, its wheels no longer drawn after 1.4 s (as the game
//               doesn't draw them out of view in first person): they must not be lost (mxcam.log)
//     defaults  the shipped defaults (no ini): no GL errors, GL state left alone
//   Every mode checks that the HUD is drawn after the effects (untouched) and that the GL state
//   the scene had is restored. A .bmp of the frame is saved next to the DLL copy.
//
//   Build from a Developer Command Prompt matching the DLL's bitness:
//     cl /nologo /EHsc /std:c++17 tests\postfx_test.cpp user32.lib gdi32.lib
//   Run:
//     postfx_test.exe bin\x64\Release\opengl32.dll off|neutral|dirt|night|defaults

#include <windows.h>
#include <GL/gl.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>
#include <vector>

namespace {

#define GL_FUNCTIONS(X) \
    X(wglCreateContext) X(wglMakeCurrent) X(wglDeleteContext) X(wglGetProcAddress) X(glGetError) \
    X(glGetIntegerv) X(glIsEnabled) X(glEnable) X(glDisable) X(glViewport) X(glClearColor)       \
    X(glClear) X(glReadPixels) X(glPixelStorei) X(glFinish) X(glDrawElements) X(glBlendFunc)     \
    X(glDepthMask)                                                                                \
    X(glGenTextures) X(glBindTexture) X(glTexImage2D) X(glTexParameteri) X(glGetTexParameterfv)  \
    X(glReadBuffer)

#define DECLARE(name) decltype(&::name) p_##name = nullptr;
GL_FUNCTIONS(DECLARE)
#undef DECLARE
BOOL(WINAPI* p_wglSwapBuffers)(HDC) = nullptr;

int g_failures = 0;
void Check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

using PFN_CreateShader       = GLuint(APIENTRY*)(GLenum);
using PFN_ShaderSource       = void(APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
using PFN_CompileShader      = void(APIENTRY*)(GLuint);
using PFN_CreateProgram      = GLuint(APIENTRY*)();
using PFN_AttachShader       = void(APIENTRY*)(GLuint, GLuint);
using PFN_LinkProgram        = void(APIENTRY*)(GLuint);
using PFN_GetProgramiv       = void(APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_UseProgram         = void(APIENTRY*)(GLuint);
using PFN_GetUniformLocation = GLint(APIENTRY*)(GLuint, const char*);
using PFN_UniformMatrix4fv   = void(APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*);
using PFN_Uniform3fv         = void(APIENTRY*)(GLint, GLsizei, const GLfloat*);
using PFN_Uniform2fv         = void(APIENTRY*)(GLint, GLsizei, const GLfloat*);
using PFN_Uniform1f          = void(APIENTRY*)(GLint, GLfloat);
using PFN_Uniform1i          = void(APIENTRY*)(GLint, GLint);
using PFN_UniformMatrix3fv   = void(APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*);
using PFN_BindAttribLocation = void(APIENTRY*)(GLuint, GLuint, const char*);
using PFN_GenObjects         = void(APIENTRY*)(GLsizei, GLuint*);
using PFN_BindObject         = void(APIENTRY*)(GLuint);
using PFN_BindBuffer         = void(APIENTRY*)(GLenum, GLuint);
using PFN_BufferData         = void(APIENTRY*)(GLenum, ptrdiff_t, const void*, GLenum);
using PFN_VertexAttribPointer = void(APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using PFN_ActiveTexture      = void(APIENTRY*)(GLenum);

struct Api {
    PFN_CreateShader CreateShader; PFN_ShaderSource ShaderSource; PFN_CompileShader CompileShader;
    PFN_CreateProgram CreateProgram; PFN_AttachShader AttachShader; PFN_LinkProgram LinkProgram;
    PFN_GetProgramiv GetProgramiv; PFN_UseProgram UseProgram; PFN_GetUniformLocation GetUniformLocation;
    PFN_UniformMatrix4fv UniformMatrix4fv; PFN_Uniform3fv Uniform3fv; PFN_Uniform2fv Uniform2fv;
    PFN_Uniform1f Uniform1f; PFN_BindAttribLocation BindAttribLocation; PFN_GenObjects GenVertexArrays;
    PFN_BindObject BindVertexArray; PFN_GenObjects GenBuffers; PFN_BindBuffer BindBuffer;
    PFN_BufferData BufferData; PFN_VertexAttribPointer VertexAttribPointer;
    PFN_BindObject EnableVertexAttribArray; PFN_ActiveTexture ActiveTexture;
    PFN_Uniform1i Uniform1i; PFN_UniformMatrix3fv UniformMatrix3fv;
};
Api gl{};

GLuint Build(const char* vertex, const char* fragment, const char* attribute)
{
    const GLuint vs = gl.CreateShader(0x8B31), fs = gl.CreateShader(0x8B30);
    gl.ShaderSource(vs, 1, &vertex, nullptr);
    gl.ShaderSource(fs, 1, &fragment, nullptr);
    gl.CompileShader(vs);
    gl.CompileShader(fs);
    const GLuint program = gl.CreateProgram();
    gl.AttachShader(program, vs);
    gl.AttachShader(program, fs);
    gl.BindAttribLocation(program, 0, attribute);
    gl.LinkProgram(program);
    GLint linked = 0;
    gl.GetProgramiv(program, 0x8B82, &linked);
    return linked ? program : 0;
}

// Column-major helpers.
void Mul(const float a[16], const float b[16], float out[16])
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            double s = 0;
            for (int k = 0; k < 4; ++k)
                s += double(a[k * 4 + r]) * b[c * 4 + k];
            out[c * 4 + r] = float(s);
        }
}

// Camera at eye, looking along -z turned by yawDeg (+ = left), pitched down by pitchDeg.
void MakeView(float eyeX, float eyeY, float eyeZ, float pitchDeg, float yawDeg, float out[16])
{
    const float p = pitchDeg * 3.14159265f / 180.0f, c = std::cos(p), s = std::sin(p);
    const float y = yawDeg * 3.14159265f / 180.0f, cy = std::cos(y), sy = std::sin(y);
    // World -> camera: Rx(+pitch) Ry(-yaw) (the camera itself is turned by Ry(yaw) Rx(-pitch):
    // looking down), after translating by -eye.
    const float r[16] = { 1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1 };
    const float ry[16] = { cy, 0, sy, 0, 0, 1, 0, 0, -sy, 0, cy, 0, 0, 0, 0, 1 };
    const float t[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -eyeX, -eyeY, -eyeZ, 1 };
    float turned[16];
    Mul(r, ry, turned);
    Mul(turned, t, out);
}

struct Pixel { int r, g, b; };

bool WriteBmp(const std::wstring& path, const std::vector<unsigned char>& rgb, int w, int h)
{
    const int stride = (w * 3 + 3) & ~3;
    std::vector<unsigned char> rows(static_cast<size_t>(stride) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 3; ++k)
                rows[static_cast<size_t>(y) * stride + x * 3 + k] = rgb[(static_cast<size_t>(y) * w + x) * 3 + 2 - k];
    BITMAPFILEHEADER file = {};
    BITMAPINFOHEADER info = {};
    info.biSize = sizeof info;
    info.biWidth = w;
    info.biHeight = h;
    info.biPlanes = 1;
    info.biBitCount = 24;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof file + sizeof info;
    file.bfSize = file.bfOffBits + static_cast<DWORD>(rows.size());
    FILE* out = nullptr;
    if (_wfopen_s(&out, path.c_str(), L"wb") != 0 || !out)
        return false;
    fwrite(&file, sizeof file, 1, out);
    fwrite(&info, sizeof info, 1, out);
    fwrite(rows.data(), 1, rows.size(), out);
    fclose(out);
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        printf("usage: postfx_test <opengl32.dll> off|neutral|dirt|night|defaults\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string mode = argv[2];

    // A folder of our own with the DLL and an ini for the mode.
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring dir = std::wstring(temp) + L"mxcam_postfx_test_" + std::wstring(mode.begin(), mode.end()) + L"\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t source[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, source, MAX_PATH);
    const std::wstring dll = dir + L"opengl32.dll";
    if (!CopyFileW(source, dll.c_str(), FALSE)) {
        printf("could not copy %s\n", argv[1]);
        return 1;
    }
    const std::wstring ini = dir + L"mxcam.ini";
    DeleteFileW(ini.c_str());
    const char* neutral =
        "[Graphics]\nShaders=1\nTimeOfDay=-1\nShadows=0\nAmbientOcclusion=0\nSunRays=0\nBloom=0\nExposure=0\n"
        "Contrast=0\nSaturation=0\nVignette=0\nAntiAliasing=0\nSharpen=0\nDirtDetail=0\nMotionBlur=0\nDepthOfField=0\n"
        "Headlight=0\nTireMarks=0\nTireDirt=0\n";
    std::string settings, base = neutral;
    if (mode == "off")
        settings = "[Graphics]\nShaders=0\n";
    else if (mode == "neutral")
        settings = base;
    else if (mode == "dirt")
        settings = base.replace(base.find("DirtDetail=0"), 12, "DirtDetail=1");
    else if (mode == "night")
        settings = base.replace(base.find("TimeOfDay=-1"), 12, "TimeOfDay=0");
    else if (mode == "shadows")  // screen-space shadows alone
        settings = base.replace(base.find("Shadows=0"), 9, "Shadows=1\nShadowMap=0");
    else if (mode == "offscreen")  // shadow maps alone (the screen-space ones as short as they go)
        settings = base.replace(base.find("Shadows=0"), 9, "Shadows=1\nShadowMap=1\nShadowLength=1");
    else if (mode == "headlight" || mode == "narrowbeam" || mode == "thirdperson") {
        base.replace(base.find("TimeOfDay=-1"), 12, "TimeOfDay=0");
        settings = base.replace(base.find("Headlight=0"), 11,
                                mode == "narrowbeam" ? "Headlight=1\nHeadlightBeamDeg=8" : "Headlight=1\nHeadlightBeamDeg=22");
    }
    else if (mode == "shadowperf" || mode == "shadowperf0")  // timing: 800 objects cast shadow-map shadows
        settings = base.replace(base.find("Shadows=0"), 9, mode == "shadowperf" ? "Shadows=1\nShadowMap=1\nShadowBatch=1"
                                                                                : "Shadows=1\nShadowMap=1\nShadowBatch=0");
    else if (mode == "tires" || mode == "tiresfeet" || mode == "tiresair" || mode == "tireshidden")  // your bike on spinning wheels (metres, feet, in the air, wheels not drawn)
        settings = base.replace(base.find("TireMarks=0\nTireDirt=0"), 22, "TireMarks=1\nTireDirt=1");
    else if (mode == "dof")
        settings = base.replace(base.find("DepthOfField=0"), 14, "DepthOfField=1\nDofFocus=60");
    else if (mode == "motion")  // the helmet cam off: its smoothing would take most of the turning out
        settings = "[General]\nEnabled=0\n" + base.replace(base.find("MotionBlur=0"), 12, "MotionBlur=1");
    else if (mode != "defaults") {
        printf("unknown mode %s\n", mode.c_str());
        return 2;
    }
    if (mode == "tireshidden")  // and shadow maps: the wheels the game stops drawing still cast
        settings = settings.replace(settings.find("Shadows=0"), 9, "Shadows=1\nShadowMap=1");
    if (!settings.empty()) {
        FILE* out = nullptr;
        if (_wfopen_s(&out, ini.c_str(), L"wb") == 0 && out) {
            fputs(settings.c_str(), out);
            fclose(out);
        }
    }

    HMODULE proxy = LoadLibraryW(dll.c_str());
    if (!proxy) {
        printf("could not load the DLL (error %lu)\n", GetLastError());
        return 1;
    }
#define LOAD(name) p_##name = reinterpret_cast<decltype(p_##name)>(GetProcAddress(proxy, #name)); \
    if (!p_##name) { printf("missing export %s\n", #name); return 1; }
    GL_FUNCTIONS(LOAD)
    LOAD(wglSwapBuffers)
#undef LOAD

    const int W = 960, H = 540;
    WNDCLASSA wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "MXCamPostfxTest";
    RegisterClassA(&wc);
    RECT frame = { 0, 0, W, H };
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowA(wc.lpszClassName, "MXCam postfx test", WS_OVERLAPPEDWINDOW, 0, 0, frame.right - frame.left,
                                frame.bottom - frame.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = p_wglCreateContext(dc);
    if (!rc || !p_wglMakeCurrent(dc, rc)) {
        printf("no GL context\n");
        return 1;
    }
#define PROC(field, name) gl.field = reinterpret_cast<decltype(gl.field)>(p_wglGetProcAddress(name)); \
    if (!gl.field) { printf("missing %s\n", name); return 1; }
    PROC(CreateShader, "glCreateShader") PROC(ShaderSource, "glShaderSource") PROC(CompileShader, "glCompileShader")
    PROC(CreateProgram, "glCreateProgram") PROC(AttachShader, "glAttachShader") PROC(LinkProgram, "glLinkProgram")
    PROC(GetProgramiv, "glGetProgramiv") PROC(UseProgram, "glUseProgram") PROC(GetUniformLocation, "glGetUniformLocation")
    PROC(UniformMatrix4fv, "glUniformMatrix4fv") PROC(Uniform3fv, "glUniform3fv") PROC(Uniform2fv, "glUniform2fv")
    PROC(Uniform1f, "glUniform1f") PROC(BindAttribLocation, "glBindAttribLocation")
    PROC(Uniform1i, "glUniform1i") PROC(UniformMatrix3fv, "glUniformMatrix3fv")
    PROC(GenVertexArrays, "glGenVertexArrays") PROC(BindVertexArray, "glBindVertexArray") PROC(GenBuffers, "glGenBuffers")
    PROC(BindBuffer, "glBindBuffer") PROC(BufferData, "glBufferData") PROC(VertexAttribPointer, "glVertexAttribPointer")
    PROC(EnableVertexAttribArray, "glEnableVertexAttribArray") PROC(ActiveTexture, "glActiveTexture")
#undef PROC

    printf("postfx test, mode '%s'\n", mode.c_str());
    // The terrain: like MX Simulator's, patches of one unit mesh placed by u_meshoffset / u_meshscale.
    const GLuint terrain = Build(
        "attribute vec2 a_corner;\n"
        "uniform mat4 u_modelview; uniform mat4 u_projection; uniform float u_heights;\n"
        "uniform vec2 u_meshoffset; uniform float u_meshscale;\n"
        "void main() {\n"
        "    vec2 p = u_meshoffset + a_corner * u_meshscale;\n"
        "    gl_Position = u_projection * u_modelview * vec4(p.x, u_heights, p.y, 1.0);\n"
        "}",
        "void main() { gl_FragColor = vec4(0.45, 0.36, 0.27, 1.0); }", "a_corner");
    // Objects (and the HUD): a flat colour, lit by u_light_position so the sun is known.
    const GLuint object = Build(
        "attribute vec3 a_pos;\n"
        "uniform mat4 u_modelview; uniform mat4 u_projection; uniform vec3 u_light_position;\n"
        "varying float v_light;\n"
        "void main() { v_light = u_light_position.x * 0.0001; gl_Position = u_projection * u_modelview * vec4(a_pos, 1.0); }",
        "uniform vec3 u_color; varying float v_light; void main() { gl_FragColor = vec4(u_color + v_light, 1.0); }",
        "a_pos");
    Check(terrain && object, "test shaders compile and link");
    if (!terrain || !object)
        return 1;
    const GLint tMv = gl.GetUniformLocation(terrain, "u_modelview");
    const GLint tProj = gl.GetUniformLocation(terrain, "u_projection");
    gl.GetUniformLocation(terrain, "u_heights");
    const GLint tOffset = gl.GetUniformLocation(terrain, "u_meshoffset");
    const GLint tScale = gl.GetUniformLocation(terrain, "u_meshscale");
    // Skinned meshes, like the game's riders and bikes: v = u_bone_r (p - u_bone_c) + u_bone_t.
    // Only their uniforms matter here (nothing is drawn on screen).
    const GLuint rider = Build(
        "attribute vec3 a_pos; uniform mat4 u_modelview; uniform mat4 u_projection; uniform int u_nbones;\n"
        "uniform vec3 u_bone_c[10]; uniform vec3 u_bone_t[10]; uniform mat3 u_bone_r[10];\n"
        "void main() {\n"
        "  vec3 p = u_bone_r[9] * (a_pos - u_bone_c[9]) + u_bone_t[9] + u_bone_r[0] * u_bone_c[0] + u_bone_t[0];\n"
        "  gl_Position = u_nbones < 0 ? u_projection * u_modelview * vec4(p, 1.0) : vec4(2.0, 2.0, 2.0, 1.0); }",
        "void main() { gl_FragColor = vec4(1.0); }", "a_pos");
    const GLint rMv = rider ? gl.GetUniformLocation(rider, "u_modelview") : -1;
    const GLint rProj = rider ? gl.GetUniformLocation(rider, "u_projection") : -1;
    const GLint rCount = rider ? gl.GetUniformLocation(rider, "u_nbones") : -1;
    const GLint rCentres = rider ? gl.GetUniformLocation(rider, "u_bone_c") : -1;
    const GLint rBones = rider ? gl.GetUniformLocation(rider, "u_bone_t") : -1;
    const GLint rRotations = rider ? gl.GetUniformLocation(rider, "u_bone_r") : -1;
    const GLint oMv = gl.GetUniformLocation(object, "u_modelview");
    const GLint oProj = gl.GetUniformLocation(object, "u_projection");
    const GLint oLight = gl.GetUniformLocation(object, "u_light_position");
    const GLint oColor = gl.GetUniformLocation(object, "u_color");

    // Meshes: the unit patch (indices in a buffer, like the game's) and a 1 m cube, and a quad.
    GLuint arrays[6] = {};
    gl.GenVertexArrays(6, arrays);
    const auto mesh = [&](GLuint array, const float* vertices, size_t vertexBytes, int components,
                          const unsigned short* indices, size_t indexBytes) {
        GLuint buffers[2] = {};
        gl.BindVertexArray(array);
        gl.GenBuffers(2, buffers);
        gl.BindBuffer(0x8892, buffers[0]);
        gl.BufferData(0x8892, static_cast<ptrdiff_t>(vertexBytes), vertices, 0x88E4);
        gl.BindBuffer(0x8893, buffers[1]);
        gl.BufferData(0x8893, static_cast<ptrdiff_t>(indexBytes), indices, 0x88E4);
        gl.VertexAttribPointer(0, components, GL_FLOAT, GL_FALSE, 0, nullptr);
        gl.EnableVertexAttribArray(0);
    };
    const float patch[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    const unsigned short patchIndices[6] = { 0, 1, 2, 2, 1, 3 };
    mesh(arrays[0], patch, sizeof patch, 2, patchIndices, sizeof patchIndices);
    const float cube[24] = { -0.5f, 0, -0.5f,  0.5f, 0, -0.5f,  -0.5f, 1, -0.5f,  0.5f, 1, -0.5f,
                             -0.5f, 0, 0.5f,   0.5f, 0, 0.5f,   -0.5f, 1, 0.5f,   0.5f, 1, 0.5f };
    const unsigned short cubeIndices[36] = { 0, 2, 1, 1, 2, 3,  4, 5, 6, 5, 7, 6,  0, 4, 2, 4, 6, 2,
                                             1, 3, 5, 5, 3, 7,  2, 6, 3, 3, 6, 7,  0, 1, 4, 4, 1, 5 };
    mesh(arrays[1], cube, sizeof cube, 3, cubeIndices, sizeof cubeIndices);
    const float quad[12] = { -1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0 };
    mesh(arrays[2], quad, sizeof quad, 3, patchIndices, sizeof patchIndices);
    // A wheel: a 2 m square across its axle (its own x), standing in for the rim and tire.
    const float wheelQuad[12] = { 0, -1, -1,  0, 1, -1,  0, -1, 1,  0, 1, 1 };
    mesh(arrays[3], wheelQuad, sizeof wheelQuad, 3, patchIndices, sizeof patchIndices);
    // The same square, 1.5 m off its model's origin (MX Simulator's wheel models have their origin
    // far from the axle: it circles the axle as the wheel turns).
    const float wheelQuadOff[12] = { 0, 0.5f, -1,  0, 2.5f, -1,  0, 0.5f, 1,  0, 2.5f, 1 };
    mesh(arrays[4], wheelQuadOff, sizeof wheelQuadOff, 3, patchIndices, sizeof patchIndices);
    // The same in feet (MX Simulator's unit): 3.28 times as big.
    const float ft = 3.2808f;
    const float wheelQuadFeet[12] = { 0, 0.5f * ft, -ft,  0, 2.5f * ft, -ft,  0, 0.5f * ft, ft,  0, 2.5f * ft, ft };
    mesh(arrays[5], wheelQuadFeet, sizeof wheelQuadFeet, 3, patchIndices, sizeof patchIndices);
    gl.BindVertexArray(0);

    // A texture the game would filter smoothly (for texture filtering), bound on unit 2 as
    // "game state" that the effects must leave alone.
    GLuint gameTexture = 0;
    p_glGenTextures(1, &gameTexture);
    p_glBindTexture(GL_TEXTURE_2D, gameTexture);
    const unsigned char texel[4] = { 255, 255, 255, 255 };
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);

    // 60 degrees vertical field of view, near 0.1, far 1000.
    const float n = 0.1f, f = 1000.0f, cot = 1.0f / std::tan(30.0f * 3.14159265f / 180.0f);
    const float projection[16] = { cot * H / W, 0, 0, 0, 0, cot, 0, 0, 0, 0, (f + n) / (n - f), -1, 0, 0, 2 * f * n / (n - f), 0 };
    float view[16];
    MakeView(0, 1.6f, 0, 12.0f, 0.0f, view);
    // What the game sends (eye space, toward the sun). For the shadow check: from the right, so
    // the box's shadow falls on the ground to its left.
    // For the shadow map check: low, from behind, so a tower behind the camera (never on screen)
    // throws its shadow forward onto the visible ground.
    const float sunEye[3] = { mode == "shadows" ? 0.8f : mode == "offscreen" ? 0.0f : 0.3f,
                              mode == "shadows" ? 0.6f : mode == "offscreen" ? 0.15f : 0.8f,
                              mode == "shadows" ? 0.0f : mode == "offscreen" ? 0.99f : 0.5f };
    const float hud[16] = { 0.05f, 0, 0, 0, 0, 0.05f, 0, 0, 0, 0, 0.05f, 0, 0, 0.3f, -1, 1 };  // scaled: the HUD
    const float flare[16] = { 0.05f, 0, 0, 0, 0, 0.05f, 0, 0, 0, 0, 0.05f, 0, -0.3f, 0.3f, -1, 1 };  // the game's lens flare
    const int flareX = static_cast<int>((1.0f - projection[0] * 0.3f) * 0.5f * W);

    std::vector<unsigned char> rgb(static_cast<size_t>(W) * H * 3);
    GLint programAfter = 0, activeAfter = 0, unit2After = 0, drawFbAfter = -1, viewportAfter[4] = {};
    bool depthAfter = true, blendAfter = false, cullAfter = true;
    GLenum errorAfter = GL_NO_ERROR;
    // A skinned draw: n bones (centres, positions, rotations) drawn with modelview.
    const auto Skinned = [&](const float* modelview, int n, const float* centres, const float* positions,
                             const float* rotations) {
        gl.UseProgram(rider);
        gl.UniformMatrix4fv(rProj, 1, GL_FALSE, projection);
        gl.UniformMatrix4fv(rMv, 1, GL_FALSE, modelview);
        gl.Uniform1i(rCount, n);
        gl.Uniform3fv(rCentres, n, centres);
        gl.Uniform3fv(rBones, n, positions);
        gl.UniformMatrix3fv(rRotations, n, GL_FALSE, rotations);
        gl.BindVertexArray(arrays[2]);
        p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    };
    // A rider with 10 bones (torso and hips on the midline, arms and legs out to the sides),
    // facing along rotation's -Z, the middle of its bones at (x, y, z). Drawn with the bare view:
    // its bones are world positions.
    const auto Rider = [&](const float rotation[9], float x, float y, float z, float scale = 1.0f) {
        const float centres[30] = { 0, 1.3f, 0,  0, 0.95f, 0,  0.25f, 1.2f, -0.1f,  -0.25f, 1.2f, -0.1f,
                                    0.3f, 1.0f, -0.35f,  -0.3f, 1.0f, -0.35f,  0.15f, 0.8f, -0.25f,  -0.15f, 0.8f, -0.25f,
                                    0.18f, 0.45f, -0.3f,  -0.18f, 0.45f, -0.3f };
        float mean[3] = {};
        for (int b = 0; b < 10; ++b)
            for (int k = 0; k < 3; ++k)
                mean[k] += centres[b * 3 + k] / 10.0f;
        float positions[30], rotations[90];
        for (int b = 0; b < 10; ++b) {
            const float c[3] = { (centres[b * 3] - mean[0]) * scale, (centres[b * 3 + 1] - mean[1]) * scale,
                                 (centres[b * 3 + 2] - mean[2]) * scale };
            for (int k = 0; k < 3; ++k)  // rotation (column-major) * c + the middle
                positions[b * 3 + k] = rotation[k] * c[0] + rotation[3 + k] * c[1] + rotation[6 + k] * c[2] +
                                       (k == 0 ? x : k == 1 ? y : z);
            for (int k = 0; k < 9; ++k)
                rotations[b * 9 + k] = rotation[k];
        }
        Skinned(view, 10, centres, positions, rotations);
    };

    const bool timing = mode == "shadowperf" || mode == "shadowperf0";
    const bool hiddenWheels = mode == "tireshidden";
    const int frames = timing ? 160 : hiddenWheels ? 200 : 12;
    LARGE_INTEGER frequency, frameStart, frameEnd, runStart;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&runStart);
    double timedMs = 0;
    int timedFrames = 0;
    for (int frameIndex = 0; frameIndex < frames; ++frameIndex) {
        QueryPerformanceCounter(&frameStart);
        if (mode == "motion")  // the head turning quickly from side to side
            MakeView(0, 1.6f, 0, 12.0f, frameIndex % 2 ? 1.0f : -1.0f, view);
        p_glViewport(0, 0, W, H);
        p_glClearColor(0.40f, 0.60f, 0.85f, 1.0f);
        p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        p_glEnable(GL_DEPTH_TEST);
        p_glDisable(GL_BLEND);
        p_glDisable(GL_CULL_FACE);

        // Terrain: an 8 x 8 grid of 25 m patches around the camera. Its first u_modelview is the camera.
        gl.UseProgram(terrain);
        gl.UniformMatrix4fv(tMv, 1, GL_FALSE, view);
        gl.UniformMatrix4fv(tProj, 1, GL_FALSE, projection);
        gl.BindVertexArray(arrays[0]);
        for (int z = -4; z < 4; ++z) {
            for (int x = -4; x < 4; ++x) {
                const float offset[2] = { x * 25.0f, z * 25.0f };
                gl.Uniform2fv(tOffset, 1, offset);
                gl.Uniform1f(tScale, 25.0f);
                p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
            }
        }

        // Third person: your rider a few metres ahead of the camera, riding away from it.
        if (mode == "thirdperson" && rider) {
            // Like a respawn at the start gate: it drops onto the gate, then sits still. The drop
            // mustn't aim the light at the ground. It faces 30 degrees right, like its bike.
            const float drop = frameIndex < 6 ? (6 - frameIndex) * 0.3f : 0.0f;
            const float turned[9] = { 0.866f, 0, 0.5f,  0, 1, 0,  -0.5f, 0, 0.866f };
            Rider(turned, 0.0f, 0.9f + drop, -2.8f);
        }

        // Third person: your bike under the rider, turned 30 degrees to the right of the camera
        // (its origin sits below the ground, like MX Simulator's bikes, so it isn't seen).
        if (mode == "thirdperson") {
            gl.UseProgram(object);
            gl.UniformMatrix4fv(oProj, 1, GL_FALSE, projection);
            const float bikeModel[16] = { 0.866f, 0, 0.5f, 0,  0, 1, 0, 0,  -0.5f, 0, 0.866f, 0,  -0.425f, -1.5f, -2.064f, 1 };
            float bikeView[16];
            Mul(view, bikeModel, bikeView);
            gl.UniformMatrix4fv(oMv, 1, GL_FALSE, bikeView);
            gl.BindVertexArray(arrays[1]);
            p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
        }

        // Tires: your bike crossing the view from left to right, 4.5 m ahead, on spinning wheels,
        // with your rider on it. The bike model faces its own -Z (here: world +X), its origin
        // sits below the ground and its wheels are models of their own, like MX Simulator's.
        if ((mode == "tires" || mode == "tiresfeet" || mode == "tiresair" || hiddenWheels) && rider) {
            const float u = mode == "tiresfeet" ? 3.2808f : 1.0f;  // game units per metre
            const float lift = mode == "tiresair" ? 1.2f : 0.0f;    // in the air: no ruts
            // tireshidden: on the clock at 5 m/s; after 1.4 s the wheels aren't drawn any more (the
            // frame and your rider still are).
            LARGE_INTEGER nowCount;
            QueryPerformanceCounter(&nowCount);
            const float elapsed = static_cast<float>(double(nowCount.QuadPart - runStart.QuadPart) / double(frequency.QuadPart));
            const bool wheelsDrawn = !hiddenWheels || elapsed < 1.4f;
            const float travelled = hiddenWheels ? 5.0f * elapsed : frameIndex * 0.45f;
            // First, like a real track: lots of other skinned objects drawn further away (more than
            // the mod used to keep track of in a frame).
            for (int k = 0; k < 100; ++k) {
                const float place[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  -15.0f + k * 0.3f, 0, -30.0f * u, 1 };
                float placed[16];
                Mul(view, place, placed);
                const float none[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
                Skinned(placed, 0, none, none, none);
            }
            const float x0 = (-5.5f + travelled) * u;
            const float facingX[9] = { 0, 0, 1,  0, 1, 0,  -1, 0, 0 };  // its -Z along world +X
            Rider(facingX, x0 + 0.85f * u, 0.9f * u + lift, -4.5f * u, u);
            const float angle = -travelled / 0.35f;
            const float c = std::cos(angle), sn = std::sin(angle);
            const float bikeModel[16] = { 0, 0, 1, 0,  0, 1, 0, 0,  -1, 0, 0, 0,  x0, -1.5f * u + lift, -4.5f * u, 1 };
            // The bike: a skinned mesh in a model of its own, its bones in it: the frame and the
            // two wheels (at the axles), which turn as it rolls.
            {
                float bikeView[16];
                Mul(view, bikeModel, bikeView);
                // The wheel bones' own centres sit 1.5 m below their axles and circle them.
                const float bikeCentres[9] = { 0, 1.9f * u, -1.0f * u,  0, 0.36f * u, -0.25f * u,  0, 0.36f * u, -1.73f * u };
                const float bikeBones[9] = { 0, 1.9f * u, -1.0f * u,  0, (1.86f - 1.5f * c) * u, (-0.25f - 1.5f * sn) * u,
                                             0, (1.86f - 1.5f * c) * u, (-1.73f - 1.5f * sn) * u };
                const float bikeRotations[27] = { 1, 0, 0, 0, 1, 0, 0, 0, 1,
                                                  1, 0, 0, 0, c, sn, 0, -sn, c,
                                                  1, 0, 0, 0, c, sn, 0, -sn, c };
                // tireshidden: only the frame is skinned; the wheels are rigid draws of their own, like the game's.
                Skinned(bikeView, wheelsDrawn && !hiddenWheels ? 3 : 1, bikeCentres, bikeBones, bikeRotations);
            }
            gl.UseProgram(object);
            gl.UniformMatrix4fv(oProj, 1, GL_FALSE, projection);
            float viewModel[16];
            Mul(view, bikeModel, viewModel);
            gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
            gl.BindVertexArray(arrays[1]);
            p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            // Something that creeps round now and then, off the centreline between the wheels (like
            // the chain or a brake disc): it must not be taken for a wheel. (Feet only: in metres
            // the test cube would be the size of the bike.)
            if (u > 1.5f) {
                const float creep = frameIndex * 0.03f, cc = std::cos(creep), cs = std::sin(creep);
                const float onBike[16] = { 1, 0, 0, 0,  0, cc, cs, 0,  0, -cs, cc, 0,  0.25f * u, 1.9f * u, -1.0f * u, 1 };
                float partModel[16];
                Mul(bikeModel, onBike, partModel);
                Mul(view, partModel, viewModel);
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            }
            const float grey[3] = { 0.3f, 0.3f, 0.3f };
            gl.Uniform3fv(oColor, 1, grey);
            for (int w = 0; w < 2 && wheelsDrawn; ++w) {
                // In the bike's space, like MX Simulator's: the rider between the wheels.
                const float axleZ = (w == 0 ? -0.25f : -1.73f) * u;
                // bike * translate(axle) * rotateX(angle) * translate(0, -1.5 m, 0): its origin circles the axle
                const float onBike[16] = { 1, 0, 0, 0,  0, c, sn, 0,  0, -sn, c, 0,  0, (1.86f - 1.5f * c) * u, axleZ - 1.5f * sn * u, 1 };
                float wheelModel[16];
                Mul(bikeModel, onBike, wheelModel);
                Mul(view, wheelModel, viewModel);
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
                gl.BindVertexArray(arrays[u > 1.5f ? 5 : 4]);
                p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
            }
        }

        // Timing: a field of 800 small boxes around the camera, every one a shadow caster.
        if (timing) {
            gl.UseProgram(object);
            gl.UniformMatrix4fv(oProj, 1, GL_FALSE, projection);
            gl.Uniform3fv(oLight, 1, sunEye);
            gl.BindVertexArray(arrays[1]);
            for (int k = 0; k < 800; ++k) {
                const float bx = -30.0f + (k % 40) * 1.5f, bz = -3.0f - (k / 40) * 1.5f;
                const float box[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  bx, 0, bz, 1 };
                float boxView[16];
                Mul(view, box, boxView);
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, boxView);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            }
        }

        // An object 6 m ahead, 1 m cube, red.
        gl.UseProgram(object);
        gl.UniformMatrix4fv(oProj, 1, GL_FALSE, projection);
        gl.Uniform3fv(oLight, 1, sunEye);
        const float model[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0f, 0, -6.0f, 1 };
        float viewModel[16];
        Mul(view, model, viewModel);
        gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
        const float red[3] = { 0.8f, 0.1f, 0.1f };
        gl.Uniform3fv(oColor, 1, red);
        gl.BindVertexArray(arrays[1]);
        p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
        if (mode == "offscreen") {  // a 2 m tower of two boxes, 0.8 m behind the camera and 1 m right
            // Drawn with blending on, as MX Simulator draws its solid objects: it must still cast.
            p_glEnable(GL_BLEND);
            p_glBlendFunc(GL_ONE, GL_ZERO);
            for (int level = 0; level < 2; ++level) {
                const float behind[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1.0f, static_cast<float>(level), 0.8f, 1 };
                Mul(view, behind, viewModel);
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            }
            // Kicked-up dirt: a see-through cloud (blended, not writing depth) just as tall, on the
            // other side. It must not cast a solid shadow onto the sunny ground.
            p_glEnable(GL_BLEND);
            p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            p_glDepthMask(GL_FALSE);
            for (int level = 0; level < 2; ++level) {
                const float dust[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1.0f, static_cast<float>(level), 0.8f, 1 };
                Mul(view, dust, viewModel);
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            }
            p_glDepthMask(GL_TRUE);
            p_glDisable(GL_BLEND);
        }

        // Game state the effects must not disturb.
        gl.ActiveTexture(0x84C0 + 2);
        p_glBindTexture(GL_TEXTURE_2D, gameTexture);
        p_glEnable(GL_BLEND);
        p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        p_glDisable(GL_DEPTH_TEST);

        // The HUD: a scaled u_modelview. The effects run right before this draw.
        gl.UniformMatrix4fv(oMv, 1, GL_FALSE, hud);
        const float white[3] = { 1, 1, 1 };
        gl.Uniform3fv(oColor, 1, white);
        gl.BindVertexArray(arrays[2]);
        p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
        // The game's lens flare: a sprite added onto the picture, left of the HUD.
        p_glBlendFunc(GL_ONE, GL_ONE);
        gl.UniformMatrix4fv(oMv, 1, GL_FALSE, flare);
        const float purple[3] = { 0.35f, 0.2f, 0.5f };
        gl.Uniform3fv(oColor, 1, purple);
        p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
        p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        p_glGetIntegerv(0x8B8D, &programAfter);
        p_glGetIntegerv(0x84E0, &activeAfter);
        p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit2After);
        p_glGetIntegerv(0x8CA6, &drawFbAfter);
        p_glGetIntegerv(GL_VIEWPORT, viewportAfter);
        depthAfter = p_glIsEnabled(GL_DEPTH_TEST) != GL_FALSE;
        blendAfter = p_glIsEnabled(GL_BLEND) != GL_FALSE;
        cullAfter = p_glIsEnabled(GL_CULL_FACE) != GL_FALSE;
        errorAfter = p_glGetError();
        if (errorAfter != GL_NO_ERROR)
            printf("    GL error 0x%X on frame %d\n", errorAfter, frameIndex);

        if (frameIndex == 11) {
            p_glFinish();
            p_glReadBuffer(GL_BACK);
            p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
            p_glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
        }
        gl.ActiveTexture(0x84C0);
        p_glDisable(GL_BLEND);
        p_wglSwapBuffers(dc);
        if (timing) {
            p_glFinish();
            QueryPerformanceCounter(&frameEnd);
            if (frameIndex >= 40) {  // after warming up
                timedMs += 1000.0 * double(frameEnd.QuadPart - frameStart.QuadPart) / double(frequency.QuadPart);
                ++timedFrames;
            }
        } else {
            Sleep(15);
        }
    }

    const auto At = [&](int x, int y) {  // y from the top
        const unsigned char* p = &rgb[(static_cast<size_t>(H - 1 - y) * W + x) * 3];
        return Pixel{ p[0], p[1], p[2] };
    };
    const auto Near = [](Pixel a, Pixel b, int tolerance) {
        return std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance && std::abs(a.b - b.b) <= tolerance;
    };
    const Pixel sky = At(W / 2, 20), ground = At(W / 2, H - 30), box = At(W / 2, 256), hudPixel = At(W / 2, 130);
    // The variation over a block of near ground: flat colour unless the dirt detail is on.
    double sum = 0, sumSq = 0;
    int count = 0;
    for (int y = H - 80; y < H - 20; ++y)
        for (int x = W / 2 - 200; x < W / 2 + 200; ++x) {
            const Pixel p = At(x, y);
            sum += p.r;
            sumSq += double(p.r) * p.r;
            ++count;
        }
    const double mean = sum / count, spread = std::sqrt(std::fmax(sumSq / count - mean * mean, 0.0));
    printf("    sky %d %d %d, ground %d %d %d, box %d %d %d, HUD %d %d %d; near ground red %.1f +- %.2f\n", sky.r, sky.g,
           sky.b, ground.r, ground.g, ground.b, box.r, box.g, box.b, hudPixel.r, hudPixel.g, hudPixel.b, mean, spread);

    const Pixel drawnSky{ 102, 153, 217 }, drawnGround{ 115, 92, 69 }, drawnBox{ 204, 26, 26 };
    Check(hudPixel.r == 255 && hudPixel.g == 255 && hudPixel.b == 255, "the HUD is drawn after the effects, untouched");
    Check(static_cast<GLuint>(programAfter) == object, "the current program is the game's again");
    Check(activeAfter == 0x84C0 + 2 && static_cast<GLuint>(unit2After) == gameTexture, "texture unit and its texture restored");
    Check(drawFbAfter == 0 && viewportAfter[2] == W && viewportAfter[3] == H, "framebuffer and viewport restored");
    Check(!depthAfter && blendAfter && !cullAfter, "depth test / blend / culling restored");
    Check(errorAfter == GL_NO_ERROR, "no GL errors");
    {
        const Pixel flarePixel = At(flareX, 130), beside = At(flareX - 45, 130);
        const bool timeOfDay = mode == "night" || mode == "headlight" || mode == "narrowbeam" || mode == "thirdperson";
        printf("    lens flare %d %d %d, beside it %d %d %d\n", flarePixel.r, flarePixel.g, flarePixel.b, beside.r, beside.g, beside.b);
        if (timeOfDay)
            Check(Near(flarePixel, beside, 6), "the game's lens flare is hidden under a time of day of our own");
        else
            Check(flarePixel.r > beside.r + 40 || flarePixel.r == 255, "the game's lens flare is drawn as usual");
    }

    if (mode == "off" || mode == "neutral") {
        const int tolerance = mode == "off" ? 1 : 2;
        Check(Near(sky, drawnSky, tolerance) && Near(ground, drawnGround, tolerance) && Near(box, drawnBox, tolerance) &&
              Near(At(20, 20), drawnSky, tolerance) && Near(At(W - 20, 20), drawnSky, tolerance) &&
              Near(At(20, H - 20), drawnGround, tolerance) && Near(At(W - 20, H - 20), drawnGround, tolerance),
              mode == "off" ? "shaders off: the picture is exactly what was drawn"
                            : "every effect at zero: the pipeline gives the picture back unchanged");
    } else if (mode == "dirt") {
        Check(spread > 2.0, "the near ground has detail");
        Check(Near(box, drawnBox, 2), "the object is left alone (the ground mask works)");
        Check(Near(sky, drawnSky, 2), "the sky is left alone");
    } else if (mode == "night") {
        // Corners on both sides of the screen's diagonals: every pass must cover the whole window.
        const Pixel skyLeft = At(20, 20), skyRight = At(W - 20, 20), groundLeft = At(20, H - 20), groundRight = At(W - 20, H - 20);
        Check(ground.r < 45 && ground.r < drawnGround.r / 2, "the ground is dark");
        Check(sky.b > sky.r && sky.r < 40 && sky.b < 80, "the sky is a dark night blue");
        Check(skyLeft.r < 40 && skyRight.r < 40 && groundLeft.r < 45 && groundRight.r < 45 && box.r < 100,
              "the whole picture is graded, to every corner");
        // The game's sun was turned: what reached the shader isn't what the game sent.
        using PFN_GetUniformfv = void(APIENTRY*)(GLuint, GLint, GLfloat*);
        const auto getUniform = reinterpret_cast<PFN_GetUniformfv>(p_wglGetProcAddress("glGetUniformfv"));
        GLfloat light[4] = {};
        if (getUniform)
            getUniform(object, oLight, light);
        printf("    light sent (%.2f %.2f %.2f), the shader got (%.2f %.2f %.2f)\n", sunEye[0], sunEye[1], sunEye[2],
               light[0], light[1], light[2]);
        const float length = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
        Check(oLight >= 0 && length > 0.5f &&
                  std::fabs(light[0] - sunEye[0]) + std::fabs(light[1] - sunEye[1]) + std::fabs(light[2] - sunEye[2]) > 0.1f,
              "the time of day moved the game's sun");
    } else if (mode == "dof" || mode == "motion") {
        // Across the middle of the box: a sharp edge goes from ground to red in a pixel or two.
        int partial = 0;
        for (int x = W / 2 - 120; x < W / 2 + 120; ++x) {
            const Pixel p = At(x, 256);
            partial += p.r > 130 && p.r < 190 ? 1 : 0;
        }
        printf("    %d partly red pixels across the box's edges\n", partial);
        Check(partial >= 8, mode == "dof" ? "the box (out of focus) has soft edges" : "the box is smeared by the turning camera");
        Check(hudPixel.r == 255, "the HUD stays sharp");
    } else if (mode == "offscreen") {
        // Ground 3.5 m ahead and 1 m right (in the tower's shadow) against 1 m left (in the sun).
        const Pixel shaded = At(604, 374), sunny = At(356, 374);
        printf("    ground right %d %d %d (tower's shadow), left %d %d %d\n", shaded.r, shaded.g, shaded.b, sunny.r, sunny.g, sunny.b);
        Check(shaded.r + 25 < sunny.r, "something off screen casts a shadow (shadow map)");
        Check(Near(sunny, drawnGround, 3), "ground in the sun stays as it was (see-through dust casts no block)");
        Check(Near(box, drawnBox, 3), "the object in front isn't shadowed by itself");
    } else if (mode == "headlight") {
        const Pixel pool = At(W / 2, 330), side = At(30, 330), corner = At(W - 20, H - 20);
        printf("    headlight pool %d %d %d, beside it %d %d %d, corner %d %d %d\n", pool.r, pool.g, pool.b, side.r, side.g,
               side.b, corner.r, corner.g, corner.b);
        Check(pool.r > 90 && pool.r > side.r * 2, "the headlight lights the ground ahead");
        Check(side.r < 45 && corner.r < 45, "outside the beam it stays night");
        Check(sky.r < 40, "the headlight doesn't light the sky");
    } else if (mode == "narrowbeam") {
        // A narrow beam is skinnier, but still lights the ground from the plate out.
        const Pixel ahead = At(W / 2, 330), closeBy = At(W / 2, H - 40), aside = At(W / 2 + 110, 330);
        printf("    narrow beam: ahead %d, near the bike %d, beside it %d\n", ahead.r, closeBy.r, aside.r);
        Check(ahead.r > 90 && closeBy.r > 90, "a narrow beam still lights the ground from the plate out");
        const Pixel underPlate = At(W / 2, H - 8);
        printf("    right in front of the plate %d\n", underPlate.r);
        Check(underPlate.r > 60, "the light starts right in front of the plate");
        Check(aside.r < 45, "a narrow beam is skinnier");
    } else if (timing) {
        printf("    %s: %.3f ms per frame with 800 shadow casters (%d frames)\n",
               mode == "shadowperf" ? "drawn together" : "one at a time", timedMs / timedFrames, timedFrames);
    } else if (mode == "tires" || mode == "tiresfeet" || mode == "tiresair") {
        const float u = mode == "tiresfeet" ? 3.2808f : 1.0f;
        // Where world points land on the screen (y from the top).
        const auto ToPixel = [&](float x, float y, float z, int& px, int& py) {
            const float world[4] = { x, y, z, 1 };
            float eye[4] = {}, clip[4] = {};
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k)
                    eye[r] += view[k * 4 + r] * world[k];
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k)
                    clip[r] += projection[k * 4 + r] * eye[k];
            px = static_cast<int>((clip[0] / clip[3] * 0.5f + 0.5f) * W);
            py = H - 1 - static_cast<int>((clip[1] / clip[3] * 0.5f + 0.5f) * H);
        };
        int mx = 0, my = 0, bx = 0, by = 0, tx = 0, ty = 0, ox = 0, oy = 0;
        ToPixel(-1.5f * u, 0.0f, -4.5f * u, mx, my);   // on the rear wheel's track, behind the bike
        ToPixel(-1.5f * u, 0.0f, -3.9f * u, bx, by);   // the same, 60 cm closer: untouched ground
        const float rearX = (-5.5f + 11 * 0.45f + 0.25f) * u;
        ToPixel(rearX - 0.27f * u, 0.26f * u, -4.5f * u, tx, ty);  // the lower back of the rear tire
        ToPixel(rearX, (0.36f + 0.62f) * u, -4.5f * u, ox, oy);   // the wheel's square, well above the tire
        const Pixel mark = At(mx, my), beside = At(bx, by), tire = At(tx, ty), above = At(ox, oy);
        printf("    tire track %d %d %d, beside it %d %d %d; tire %d %d %d, above it %d %d %d\n", mark.r, mark.g, mark.b,
               beside.r, beside.g, beside.b, tire.r, tire.g, tire.b, above.r, above.g, above.b);
        if (mode == "tiresair") {  // the same ride 1.2 m up: the wheels turn, nothing presses into the ground
            Check(Near(mark, drawnGround, 3), "no ruts while the bike is in the air");
        } else {
            Check(mark.r + 20 < beside.r, "the rear tire leaves a groove behind it");
            Check(Near(beside, drawnGround, 3), "the ground beside the track is untouched");
            Check(tire.r > tire.b + 25, "the tire is caked with mud");
            Check(std::abs(above.r - above.b) < 8, "higher up the bike stays clean");
        }
    } else if (hiddenWheels) {
        // What the log says about the wheels: found while drawn, and never lost after.
        std::string log;
        // The DLL still has it open for writing: share it.
        FILE* in = _wfsopen((dir + L"mxcam.log").c_str(), L"rb", _SH_DENYNO);
        if (in) {
            char buffer[4096];
            size_t got;
            while ((got = fread(buffer, 1, sizeof buffer, in)) > 0)
                log.append(buffer, got);
            fclose(in);
        }
        size_t at = 0;
        while ((at = log.find("tire marks:", at)) != std::string::npos) {
            const size_t end = log.find('\n', at);
            printf("    log: %s\n", log.substr(at, end == std::string::npos ? std::string::npos : end - at).c_str());
            at = end;
        }
        for (size_t from = 0; (from = log.find("shadow map:", from)) != std::string::npos;) {
            const size_t end = log.find('\n', from);
            printf("    log: %s\n", log.substr(from, end == std::string::npos ? std::string::npos : end - from).c_str());
            from = end;
        }
        Check(log.find("tire marks: your wheels found") != std::string::npos, "the wheels are found while they're drawn");
        Check(log.find("tire marks: your wheels lost") == std::string::npos,
              "wheels the game stops drawing stay on your bike (not lost)");
        for (const char* wheel : { "rear", "front" }) {
            const std::string kept = std::string("shadow map: kept your ") + wheel + " wheel's draw call";
            const std::string cast = std::string("shadow map: your ") + wheel + " wheel isn't drawn by the game here";
            Check(log.find(kept) != std::string::npos, (std::string("the ") + wheel + " wheel's draw call is kept while it's drawn").c_str());
            Check(log.find(cast) != std::string::npos, (std::string("the ") + wheel + " wheel still casts a shadow once it isn't drawn").c_str());
        }
    } else if (mode == "thirdperson") {
        // The light starts at the rider's bike, not at the camera behind it.
        // (The bike is turned 30 degrees to the right, so its beam is right of the middle.)
        const Pixel underCamera = At(W / 2, H - 8), pool = At(W / 2 + 150, 330);
        printf("    third person: ground in front of the camera %d, ahead of the rider %d\n", underCamera.r, pool.r);
        Check(underCamera.r < 45, "third person: the camera itself doesn't carry the light");
        Check(pool.r > 60, "third person: the light shines ahead of your bike");
        const Pixel right = At(W / 2 + 150, 330), left = At(W / 2 - 150, 330);
        printf("    third person: right of the middle %d, left %d\n", right.r, left.r);
        Check(right.r > left.r + 30, "third person: the light turns with the bike, not the rider");
    } else if (mode == "shadows") {
        // Ground just left of the box's base (in its shadow) against the same distance to its right.
        int darker = 0;
        for (int dy = 0; dy < 8; ++dy) {
            const Pixel left = At(W / 2 - 60, 296 + dy), right = At(W / 2 + 60, 296 + dy);
            darker += left.r + 10 < right.r ? 1 : 0;
        }
        printf("    beside the box: left %d, right %d\n", At(W / 2 - 60, 300).r, At(W / 2 + 60, 300).r);
        Check(darker >= 6, "the box casts a shadow on the ground, away from the sun");
        Check(Near(At(W / 2 + 200, 400), drawnGround, 3), "open ground in the sun stays as it was");
    } else if (mode == "defaults") {
        Check(Near(box, drawnBox, 40), "the defaults keep the picture recognisable");
    }

    // Texture filtering: the game's bilinear mipmapped texture became trilinear (and anisotropic).
    if (mode == "defaults" || mode == "off") {
        p_glBindTexture(GL_TEXTURE_2D, gameTexture);
        GLfloat filter = 0, anisotropy = 0;
        p_glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
        p_glGetTexParameterfv(GL_TEXTURE_2D, 0x84FE, &anisotropy);
        p_glGetError();
        printf("    game texture: min filter 0x%X, anisotropy %.0f\n", static_cast<int>(filter), anisotropy);
        if (mode == "defaults")
            Check(filter == GL_LINEAR_MIPMAP_LINEAR && anisotropy >= 2.0f, "texture filtering: trilinear and anisotropic");
        else
            Check(filter == GL_LINEAR_MIPMAP_NEAREST && anisotropy <= 1.0f, "shaders off: the game's own filtering");
    }

    WriteBmp(dir + L"frame.bmp", rgb, W, H);
    wprintf(L"    frame saved to %sframe.bmp\n", dir.c_str());
    p_wglMakeCurrent(nullptr, nullptr);
    p_wglDeleteContext(rc);
    printf(g_failures ? "%d check(s) FAILED\n" : "All checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
