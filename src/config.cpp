// config.cpp
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include "config.h"
#include "log.h"
#include "mathlib.h"

namespace mxcam {
namespace {

Config  g_config;
wchar_t g_path[1024] = L"";

// Presets: three whole sets of the settings (CopyTunables). The live settings (g_config, kept in
// the ini's usual sections) are the active preset's; the others are kept here, and in the ini in
// sections of their own ([Preset1] .. [Preset3]) with their keys as "Section.Key".
Config  g_presets[kPresetCount];
int     g_activePreset = 0;
const wchar_t* g_presetSection = nullptr;  // while reading / writing a preset's section

bool ReadRaw(const wchar_t* section, const wchar_t* key, wchar_t* buffer, DWORD size)
{
    if (g_presetSection) {
        wchar_t full[128];
        swprintf_s(full, L"%s.%s", section, key);
        GetPrivateProfileStringW(g_presetSection, full, L"", buffer, size, g_path);
    } else {
        GetPrivateProfileStringW(section, key, L"", buffer, size, g_path);
    }
    return buffer[0] != L'\0';
}

const wchar_t* PresetSection(int index)
{
    static const wchar_t* const kSections[kPresetCount] = { L"Preset1", L"Preset2", L"Preset3" };
    return kSections[index];
}

double ReadDouble(const wchar_t* section, const wchar_t* key, double fallback)
{
    wchar_t buffer[64];
    if (!ReadRaw(section, key, buffer, _countof(buffer)))
        return fallback;
    wchar_t* end = nullptr;
    const double value = wcstod(buffer, &end);
    return end != buffer ? value : fallback;
}

int ReadInt(const wchar_t* section, const wchar_t* key, int fallback)
{
    wchar_t buffer[64];
    if (!ReadRaw(section, key, buffer, _countof(buffer)))
        return fallback;
    wchar_t* end = nullptr;
    const long value = wcstol(buffer, &end, 0);  // base 0: accepts 120 or 0x78
    return end != buffer ? static_cast<int>(value) : fallback;
}

void ReadString(const wchar_t* section, const wchar_t* key, char (&value)[64])
{
    wchar_t buffer[64];
    if (!ReadRaw(section, key, buffer, _countof(buffer)))
        return;
    // Keep only the first word, so a trailing "; comment" can't sneak into the name.
    wchar_t* end = buffer;
    while (*end && *end != L' ' && *end != L'\t' && *end != L';')
        ++end;
    *end = L'\0';
    if (!WideCharToMultiByte(CP_UTF8, 0, buffer, -1, value, sizeof value, nullptr, nullptr))
        value[0] = '\0';
}

void ReadVector(const wchar_t* section, const wchar_t* key, double (&value)[3])
{
    wchar_t buffer[128];
    double x, y, z;
    if (!ReadRaw(section, key, buffer, _countof(buffer)) ||
        swscanf_s(buffer, L"%lf , %lf , %lf", &x, &y, &z) != 3)
        return;
    const double length = std::sqrt(x * x + y * y + z * z);
    if (length < 1e-9)
        return;
    value[0] = x / length;
    value[1] = y / length;
    value[2] = z / length;
}

// Every setting the presets and DEFAULTS cover (CopyTunables), written into the usual sections,
// or into a preset's own (keys "Section.Key").
bool WriteTunables(const Config& c, const wchar_t* presetSection)
{
    struct Value {
        const wchar_t* section;
        const wchar_t* key;
        double         value;
    };
    const Value values[] = {
        { L"Camera",    L"CustomCamera",         c.customCamera ? 1.0 : 0.0 },
        { L"Camera",    L"KeepGameAngle",        c.keepGameAngle ? 1.0 : 0.0 },
        { L"Camera",    L"CameraAngleDeg",       c.cameraAngleDeg },
        { L"Camera",    L"HeadForward",          c.headForward },
        { L"Camera",    L"HeadHeight",           c.headHeight },
        { L"Camera",    L"HideOwnBody",          c.hideOwnBody ? 1.0 : 0.0 },
        { L"Smoothing", L"RotationResponse",     c.rotationResponse },
        { L"Smoothing", L"MaxLagDeg",            c.maxLagDeg },
        { L"Leveling",  L"HorizonLock",          c.horizonLock },
        { L"Leveling",  L"PitchLock",            c.pitchLock },
        { L"Leveling",  L"MaxRollCorrectionDeg", c.maxRollCorrectionDeg },
        { L"Leveling",  L"Steadiness",           c.steadiness },
        { L"Air",       L"AirLooseness",         c.airLooseness },
        { L"Air",       L"WhipLock",             c.whipLock },
        { L"Look",      L"ControllerLook",       c.controllerLook ? 1.0 : 0.0 },
        { L"Look",      L"LookSideDeg",          c.lookSideDeg },
        { L"Look",      L"LookTurnSpeed",        c.lookTurnSpeed },
        { L"Look",      L"ApexLook",             c.apexLook },
        { L"Look",      L"ApexSpeed",            c.apexSpeed },
        { L"Look",      L"ApexAnticipation",     c.apexAnticipation },
        { L"Look",      L"LookSideForward",      c.lookSideForward },
        { L"Look",      L"LookSideHeight",       c.lookSideHeight },
        { L"Look",      L"LookBackForward",      c.lookBackForward },
        { L"Look",      L"LookBackHeight",       c.lookBackHeight },
        { L"Neck",      L"VerticalResponse",     c.verticalResponse },
        { L"Neck",      L"MaxVerticalLag",       c.maxVerticalLag },
        { L"Neck",      L"CompressionPerG",      c.compressionPerG },
        { L"Neck",      L"SpringHz",             c.springHz },
        { L"Neck",      L"DampingRatio",         c.dampingRatio },
        { L"Neck",      L"MaxCompression",       c.maxCompression },
        { L"Neck",      L"MaxExtension",         c.maxExtension },
        { L"Neck",      L"NodDegPerMeter",       c.nodDegPerMeter },
        { L"Neck",      L"AccelFilterHz",        c.accelFilterHz },
        { L"Replay",    L"Camera",               static_cast<double>(c.replayCamera) },
        { L"Replay",    L"Distance",             c.replayDistance },
        { L"Replay",    L"Height",               c.replayHeight },
        { L"Replay",    L"PitchDeg",             c.replayPitchDeg },
        { L"Replay",    L"HelmetHeight",         c.helmetHeight },
        { L"Replay",    L"HelmetForward",        c.helmetForward },
        { L"Replay",    L"HelmetTiltDeg",        c.helmetTiltDeg },
        { L"Replay",    L"HelmetMount",          static_cast<double>(c.helmetMount) },
        { L"Replay",    L"SideHeight",           c.sideHeight },
        { L"Replay",    L"SideForward",          c.sideForward },
        { L"Replay",    L"SideTiltDeg",          c.sideTiltDeg },
        { L"Replay",    L"SideOut",              c.sideOut },
        { L"Replay",    L"ChestHeight",          c.chestHeight },
        { L"Replay",    L"ChestForward",         c.chestForward },
        { L"Replay",    L"ChestTiltDeg",         c.chestTiltDeg },
        { L"Replay",    L"HelmetSmoothing",      c.helmetSmoothing },
        { L"Replay",    L"HelmetLevel",          c.helmetLevel },
        { L"Graphics",  L"Shaders",              c.shaders ? 1.0 : 0.0 },
        { L"Graphics",  L"TimeOfDay",            c.timeOfDay },
        { L"Graphics",  L"MoveSun",              c.moveSun ? 1.0 : 0.0 },
        { L"Graphics",  L"NightBrightness",      c.nightBrightness },
        { L"Graphics",  L"HideGameFlare",        c.hideGameFlare ? 1.0 : 0.0 },
        { L"Graphics",  L"Shadows",              c.shadows },
        { L"Graphics",  L"ShadowLength",         c.shadowLength },
        { L"Graphics",  L"ShadowMap",            c.shadowMap ? 1.0 : 0.0 },
        { L"Graphics",  L"ShadowDistanceM",      c.shadowDistance },
        { L"Graphics",  L"ShadowResolution",     static_cast<double>(c.shadowResolution) },
        { L"Graphics",  L"Headlight",            c.headlight },
        { L"Graphics",  L"HeadlightBeamDeg",     c.headlightBeamDeg },
        { L"Graphics",  L"HeadlightAimDeg",      c.headlightAimDeg },
        { L"Graphics",  L"AmbientOcclusion",     c.ambientOcclusion },
        { L"Graphics",  L"SunRays",              c.sunRays },
        { L"Graphics",  L"Bloom",                c.bloom },
        { L"Graphics",  L"Exposure",             c.exposure },
        { L"Graphics",  L"Contrast",             c.contrast },
        { L"Graphics",  L"Saturation",           c.saturation },
        { L"Graphics",  L"Vignette",             c.vignette },
        { L"Graphics",  L"AntiAliasing",         static_cast<double>(c.antiAliasing) },
        { L"Graphics",  L"Sharpen",              c.sharpen },
        { L"Graphics",  L"Anisotropy",           c.anisotropy },
        { L"Graphics",  L"Trilinear",            c.trilinear ? 1.0 : 0.0 },
        { L"Graphics",  L"TextureLodBias",       c.textureLodBias },
        { L"Graphics",  L"DirtDetail",           c.dirtDetail },
        { L"Graphics",  L"DirtScale",            c.dirtScale },
        { L"Graphics",  L"TireMarks",            c.tireMarks },
        { L"Graphics",  L"TireDirt",             c.tireDirt },
        { L"Graphics",  L"MotionBlur",           c.motionBlur },
        { L"Graphics",  L"DepthOfField",         c.depthOfField },
        { L"Graphics",  L"DofFocus",             c.dofFocus },
        { L"Other",     L"HideHud",              c.hideHud ? 1.0 : 0.0 },
        { L"Other",     L"HideCursor",           c.hideCursor ? 1.0 : 0.0 },
        { L"Other",     L"FadeMenu",             c.fadeMenu ? 1.0 : 0.0 },
    };

    bool ok = true;
    for (const Value& v : values) {
        wchar_t text[32];
        swprintf_s(text, L"%.6g", v.value);
        if (presetSection) {
            wchar_t full[128];
            swprintf_s(full, L"%s.%s", v.section, v.key);
            ok = WritePrivateProfileStringW(presetSection, full, text, g_path) != FALSE && ok;
        } else {
            ok = WritePrivateProfileStringW(v.section, v.key, text, g_path) != FALSE && ok;
        }
    }
    return ok;
}

}  // namespace

const Config& GetConfig() { return g_config; }

Config& EditConfig() { return g_config; }

void SetEnabled(bool enabled) { g_config.enabled = enabled; }

bool SaveConfig()
{
    if (!g_path[0])
        return false;

    bool ok = WritePrivateProfileStringW(L"General", L"Enabled", g_config.enabled ? L"1" : L"0", g_path) != FALSE;
    // The keybinds (the KEYBINDS tab).
    const struct { const wchar_t* section; const wchar_t* key; int vk; } keys[] = {
        { L"General",  L"MenuKey",   g_config.menuKey },
        { L"General",  L"ToggleKey", g_config.toggleKey },
        { L"Graphics", L"ToggleKey", g_config.shaderToggleKey },
    };
    for (const auto& k : keys) {
        wchar_t text[16];
        swprintf_s(text, L"0x%02X", k.vk);
        ok = WritePrivateProfileStringW(k.section, k.key, text, g_path) != FALSE && ok;
    }
    // A display choice, shared by the presets.
    ok = WritePrivateProfileStringW(L"Other", L"FullscreenWindow", g_config.fullscreenWindow ? L"1" : L"0", g_path) != FALSE && ok;
    ok = WriteTunables(g_config, nullptr) && ok;
    // The presets: which one is live (its settings are the ones above), and the others' own.
    wchar_t active[8];
    swprintf_s(active, L"%d", g_activePreset + 1);
    ok = WritePrivateProfileStringW(L"Presets", L"Active", active, g_path) != FALSE && ok;
    for (int i = 0; i < kPresetCount; ++i) {
        if (i == g_activePreset) {
            WritePrivateProfileStringW(PresetSection(i), nullptr, nullptr, g_path);  // it's the usual sections
            continue;
        }
        Config preset = g_config;
        CopyTunables(g_presets[i], preset);
        ok = WriteTunables(preset, PresetSection(i)) && ok;
    }
    // Renamed to HorizonLock; drop the old key so the file can't disagree with itself.
    WritePrivateProfileStringW(L"Leveling", L"LeanCompensation", nullptr, g_path);
    WritePrivateProfileStringW(L"Replay", L"ChaseCam", nullptr, g_path);  // now Camera
    Log("config: %s %ls", ok ? "saved" : "FAILED to save", g_path);
    return ok;
}

void CopyTunables(const Config& from, Config& to)
{
    to.customCamera         = from.customCamera;
    to.keepGameAngle        = from.keepGameAngle;
    to.cameraAngleDeg       = from.cameraAngleDeg;
    to.headForward          = from.headForward;
    to.headHeight           = from.headHeight;
    to.hideOwnBody          = from.hideOwnBody;
    to.rotationResponse     = from.rotationResponse;
    to.maxLagDeg            = from.maxLagDeg;
    to.horizonLock          = from.horizonLock;
    to.pitchLock            = from.pitchLock;
    to.maxRollCorrectionDeg = from.maxRollCorrectionDeg;
    to.steadiness           = from.steadiness;
    to.airLooseness         = from.airLooseness;
    to.whipLock             = from.whipLock;
    to.controllerLook       = from.controllerLook;
    to.lookSideDeg          = from.lookSideDeg;
    to.lookTurnSpeed        = from.lookTurnSpeed;
    to.apexLook             = from.apexLook;
    to.apexSpeed            = from.apexSpeed;
    to.apexAnticipation     = from.apexAnticipation;
    to.lookSideForward      = from.lookSideForward;
    to.lookSideHeight       = from.lookSideHeight;
    to.lookBackForward      = from.lookBackForward;
    to.lookBackHeight       = from.lookBackHeight;
    to.verticalResponse     = from.verticalResponse;
    to.maxVerticalLag       = from.maxVerticalLag;
    to.compressionPerG      = from.compressionPerG;
    to.springHz             = from.springHz;
    to.dampingRatio         = from.dampingRatio;
    to.maxCompression       = from.maxCompression;
    to.maxExtension         = from.maxExtension;
    to.nodDegPerMeter       = from.nodDegPerMeter;
    to.accelFilterHz        = from.accelFilterHz;
    to.replayCamera         = from.replayCamera;
    to.replayDistance       = from.replayDistance;
    to.replayHeight         = from.replayHeight;
    to.replayPitchDeg       = from.replayPitchDeg;
    to.helmetHeight         = from.helmetHeight;
    to.helmetForward        = from.helmetForward;
    to.helmetTiltDeg        = from.helmetTiltDeg;
    to.helmetMount          = from.helmetMount;
    to.sideHeight           = from.sideHeight;
    to.sideForward          = from.sideForward;
    to.sideTiltDeg          = from.sideTiltDeg;
    to.sideOut              = from.sideOut;
    to.chestHeight          = from.chestHeight;
    to.chestForward         = from.chestForward;
    to.chestTiltDeg         = from.chestTiltDeg;
    to.helmetSmoothing      = from.helmetSmoothing;
    to.helmetLevel          = from.helmetLevel;
    to.shaders              = from.shaders;
    to.timeOfDay            = from.timeOfDay;
    to.moveSun              = from.moveSun;
    to.nightBrightness      = from.nightBrightness;
    to.hideGameFlare        = from.hideGameFlare;
    to.shadows              = from.shadows;
    to.shadowLength         = from.shadowLength;
    to.shadowMap            = from.shadowMap;
    to.shadowDistance       = from.shadowDistance;
    to.shadowResolution     = from.shadowResolution;
    to.shadowMapSize        = from.shadowMapSize;
    to.headlight            = from.headlight;
    to.headlightBeamDeg     = from.headlightBeamDeg;
    to.headlightAimDeg      = from.headlightAimDeg;
    to.ambientOcclusion     = from.ambientOcclusion;
    to.sunRays              = from.sunRays;
    to.bloom                = from.bloom;
    to.exposure             = from.exposure;
    to.contrast             = from.contrast;
    to.saturation           = from.saturation;
    to.vignette             = from.vignette;
    to.antiAliasing         = from.antiAliasing;
    to.sharpen              = from.sharpen;
    to.anisotropy           = from.anisotropy;
    to.trilinear            = from.trilinear;
    to.textureLodBias       = from.textureLodBias;
    to.dirtDetail           = from.dirtDetail;
    to.dirtScale            = from.dirtScale;
    to.tireMarks            = from.tireMarks;
    to.tireDirt             = from.tireDirt;
    to.motionBlur           = from.motionBlur;
    to.depthOfField         = from.depthOfField;
    to.dofFocus             = from.dofFocus;
    to.hideHud              = from.hideHud;
    to.hideCursor           = from.hideCursor;
    to.fadeMenu             = from.fadeMenu;
}

void ResetTunables()
{
    CopyTunables(Config{}, g_config);
}

int ActivePreset() { return g_activePreset; }

void SwitchPreset(int index)
{
    if (index < 0 || index >= kPresetCount || index == g_activePreset)
        return;
    CopyTunables(g_config, g_presets[g_activePreset]);  // the live settings stay with their preset
    CopyTunables(g_presets[index], g_config);
    g_activePreset = index;
    Log("config: preset %d", index + 1);
}

void LoadConfig(const wchar_t* iniPath)
{
    wcscpy_s(g_path, iniPath);
    ReloadConfig();
}

bool ReadLearned(const char* key, double value[3])
{
    if (!g_path[0])
        return false;
    wchar_t wkey[64] = L"", buffer[128] = L"";
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 64);
    GetPrivateProfileStringW(L"Learned", wkey, L"", buffer, 128, g_path);
    double v[3];
    if (swscanf_s(buffer, L"%lf,%lf,%lf", &v[0], &v[1], &v[2]) != 3)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > 100.0)
            return false;
        value[i] = v[i];
    }
    return true;
}

void WriteLearned(const char* key, const double value[3])
{
    if (!g_path[0])
        return;
    wchar_t wkey[64] = L"", text[128] = L"";
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 64);
    swprintf_s(text, L"%.3f,%.3f,%.3f", value[0], value[1], value[2]);
    WritePrivateProfileStringW(L"Learned", wkey, text, g_path);
}

void ForgetLearned(const char* key)
{
    if (!g_path[0])
        return;
    wchar_t wkey[64] = L"";
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 64);
    WritePrivateProfileStringW(L"Learned", wkey, nullptr, g_path);
}

namespace {

// Everything in the ini (or, with g_presetSection set, a preset's section), over base: what's
// missing keeps base's value. Every value is kept to a range the physics can't blow up with.
Config ParseConfig(const Config& base)
{
    Config c = base;
    c.enabled   = ReadInt(L"General", L"Enabled", c.enabled) != 0;
    c.toggleKey = ReadInt(L"General", L"ToggleKey", c.toggleKey);
    c.reloadKey = ReadInt(L"General", L"ReloadKey", c.reloadKey);
    c.dumpKey   = ReadInt(L"General", L"DumpKey", c.dumpKey);
    c.menuKey   = ReadInt(L"General", L"MenuKey", c.menuKey);
    c.traceKey  = ReadInt(L"General", L"TraceKey", c.traceKey);

    ReadString(L"Detection", L"ModelviewUniform", c.modelviewUniform);
    ReadString(L"Detection", L"CameraProgramUniform", c.cameraProgramUniform);
    ReadString(L"Detection", L"LightUniform", c.lightUniform);
    ReadString(L"Detection", L"EnvMatrixUniform", c.envUniform);
    c.viewMatrixIndex  = ReadInt(L"Detection", L"ViewMatrixIndex", c.viewMatrixIndex);
    ReadVector(L"Detection", L"WorldUp", c.worldUp);
    c.snapAngleDeg     = ReadDouble(L"Detection", L"SnapAngleDeg", c.snapAngleDeg);
    c.teleportDistance = ReadDouble(L"Detection", L"TeleportDistance", c.teleportDistance);
    c.bikeSearchRadius = ReadDouble(L"Detection", L"BikeSearchRadius", c.bikeSearchRadius);

    c.customCamera   = ReadInt(L"Camera", L"CustomCamera", c.customCamera) != 0;
    c.keepGameAngle  = ReadInt(L"Camera", L"KeepGameAngle", c.keepGameAngle) != 0;
    c.cameraAngleDeg = ReadDouble(L"Camera", L"CameraAngleDeg", c.cameraAngleDeg);
    c.headForward    = ReadDouble(L"Camera", L"HeadForward", c.headForward);
    c.headHeight     = ReadDouble(L"Camera", L"HeadHeight", c.headHeight);
    c.hideOwnBody    = ReadInt(L"Camera", L"HideOwnBody", c.hideOwnBody) != 0;

    c.rotationResponse = ReadDouble(L"Smoothing", L"RotationResponse", c.rotationResponse);
    c.maxLagDeg        = ReadDouble(L"Smoothing", L"MaxLagDeg", c.maxLagDeg);

    // HorizonLock was called LeanCompensation in earlier versions.
    c.horizonLock          = ReadDouble(L"Leveling", L"LeanCompensation", c.horizonLock);
    c.horizonLock          = ReadDouble(L"Leveling", L"HorizonLock", c.horizonLock);
    c.pitchLock            = ReadDouble(L"Leveling", L"PitchLock", c.pitchLock);
    c.maxRollCorrectionDeg = ReadDouble(L"Leveling", L"MaxRollCorrectionDeg", c.maxRollCorrectionDeg);
    c.steadiness           = ReadDouble(L"Leveling", L"Steadiness", c.steadiness);

    c.airLooseness     = ReadDouble(L"Air", L"AirLooseness", c.airLooseness);
    c.whipLock         = ReadDouble(L"Air", L"WhipLock", c.whipLock);

    c.controllerLook  = ReadInt(L"Look", L"ControllerLook", c.controllerLook) != 0;
    c.lookSideDeg     = ReadDouble(L"Look", L"LookSideDeg", c.lookSideDeg);
    c.lookTurnSpeed   = ReadDouble(L"Look", L"LookTurnSpeed", c.lookTurnSpeed);
    c.apexLook        = ReadDouble(L"Look", L"ApexLook", c.apexLook);
    c.apexSpeed       = ReadDouble(L"Look", L"ApexSpeed", c.apexSpeed);
    c.apexAnticipation = ReadDouble(L"Look", L"ApexAnticipation", c.apexAnticipation);
    c.lookSideForward = ReadDouble(L"Look", L"LookSideForward", c.lookSideForward);
    c.lookSideHeight  = ReadDouble(L"Look", L"LookSideHeight", c.lookSideHeight);
    c.lookBackForward = ReadDouble(L"Look", L"LookBackForward", c.lookBackForward);
    c.lookBackHeight  = ReadDouble(L"Look", L"LookBackHeight", c.lookBackHeight);

    c.verticalResponse = ReadDouble(L"Neck", L"VerticalResponse", c.verticalResponse);
    c.maxVerticalLag   = ReadDouble(L"Neck", L"MaxVerticalLag", c.maxVerticalLag);
    c.compressionPerG  = ReadDouble(L"Neck", L"CompressionPerG", c.compressionPerG);
    c.springHz         = ReadDouble(L"Neck", L"SpringHz", c.springHz);
    c.dampingRatio     = ReadDouble(L"Neck", L"DampingRatio", c.dampingRatio);
    c.maxCompression   = ReadDouble(L"Neck", L"MaxCompression", c.maxCompression);
    c.maxExtension     = ReadDouble(L"Neck", L"MaxExtension", c.maxExtension);
    c.nodDegPerMeter   = ReadDouble(L"Neck", L"NodDegPerMeter", c.nodDegPerMeter);
    c.accelFilterHz    = ReadDouble(L"Neck", L"AccelFilterHz", c.accelFilterHz);

    // Camera replaced ChaseCam (0 / 1), which older files still have.
    c.replayCamera   = ReadInt(L"Replay", L"Camera", ReadInt(L"Replay", L"ChaseCam", c.replayCamera) != 0 ? 1 : 0);
    c.replayDistance = ReadDouble(L"Replay", L"Distance", c.replayDistance);
    c.replayHeight   = ReadDouble(L"Replay", L"Height", c.replayHeight);
    c.replayPitchDeg = ReadDouble(L"Replay", L"PitchDeg", c.replayPitchDeg);
    c.helmetHeight    = ReadDouble(L"Replay", L"HelmetHeight", c.helmetHeight);
    c.helmetForward   = ReadDouble(L"Replay", L"HelmetForward", c.helmetForward);
    c.helmetTiltDeg   = ReadDouble(L"Replay", L"HelmetTiltDeg", c.helmetTiltDeg);
    c.helmetMount     = ReadInt(L"Replay", L"HelmetMount", c.helmetMount);
    c.sideHeight      = ReadDouble(L"Replay", L"SideHeight", c.sideHeight);
    c.sideForward     = ReadDouble(L"Replay", L"SideForward", c.sideForward);
    c.sideTiltDeg     = ReadDouble(L"Replay", L"SideTiltDeg", c.sideTiltDeg);
    c.sideOut         = ReadDouble(L"Replay", L"SideOut", c.sideOut);
    c.chestHeight     = ReadDouble(L"Replay", L"ChestHeight", c.chestHeight);
    c.chestForward    = ReadDouble(L"Replay", L"ChestForward", c.chestForward);
    c.chestTiltDeg    = ReadDouble(L"Replay", L"ChestTiltDeg", c.chestTiltDeg);
    c.helmetSmoothing = ReadDouble(L"Replay", L"HelmetSmoothing", c.helmetSmoothing);
    c.helmetLevel     = ReadDouble(L"Replay", L"HelmetLevel", c.helmetLevel);

    c.shaders          = ReadInt(L"Graphics", L"Shaders", c.shaders) != 0;
    c.shaderToggleKey  = ReadInt(L"Graphics", L"ToggleKey", c.shaderToggleKey);
    c.applyBeforeHud   = ReadInt(L"Graphics", L"ApplyBeforeHud", c.applyBeforeHud) != 0;
    c.timeOfDay        = ReadDouble(L"Graphics", L"TimeOfDay", c.timeOfDay);
    c.moveSun          = ReadInt(L"Graphics", L"MoveSun", c.moveSun) != 0;
    c.nightBrightness  = ReadDouble(L"Graphics", L"NightBrightness", c.nightBrightness);
    c.hideGameFlare    = ReadInt(L"Graphics", L"HideGameFlare", c.hideGameFlare) != 0;
    c.invertLight      = ReadInt(L"Graphics", L"InvertLight", c.invertLight) != 0;
    c.shadows          = ReadDouble(L"Graphics", L"Shadows", c.shadows);
    c.shadowLength     = ReadDouble(L"Graphics", L"ShadowLength", c.shadowLength);
    c.shadowMap        = ReadInt(L"Graphics", L"ShadowMap", c.shadowMap) != 0;
    // Shadow distance in real metres (ShadowDistanceM). The old ShadowDistance was used in the
    // game's own unit, feet, though it said metres: read as that, so it covers what it did.
    {
        const double metres = ReadDouble(L"Graphics", L"ShadowDistanceM", -1.0);
        const double feet = ReadDouble(L"Graphics", L"ShadowDistance", -1.0);
        if (metres >= 0.0)
            c.shadowDistance = metres;
        else if (feet >= 0.0)
            c.shadowDistance = feet / 3.2808;
    }
    {
        const int resolution = ReadInt(L"Graphics", L"ShadowResolution", -1);
        const int size = ReadInt(L"Graphics", L"ShadowMapSize", -1);  // (the old way to set it)
        if (resolution >= 0)
            c.shadowResolution = resolution;
        else if (size > 0)
            c.shadowResolution = size >= 8192 ? 2 : size >= 4096 ? 1 : 0;
    }
    c.shadowBatch      = ReadInt(L"Graphics", L"ShadowBatch", c.shadowBatch) != 0;
    c.headlight        = ReadDouble(L"Graphics", L"Headlight", c.headlight);
    c.headlightBeamDeg = ReadDouble(L"Graphics", L"HeadlightBeamDeg", c.headlightBeamDeg);
    c.headlightForward = ReadDouble(L"Graphics", L"HeadlightForward", c.headlightForward);
    c.headlightHeight  = ReadDouble(L"Graphics", L"HeadlightHeight", c.headlightHeight);
    {
        // (Before HeadlightAimDeg: HeadlightPitchDeg, how far it was aimed down, its top edge 6
        // degrees under that.)
        const double aim = ReadDouble(L"Graphics", L"HeadlightAimDeg", 1e9);
        c.headlightAimDeg = aim < 1e8 ? aim : 6.0 - ReadDouble(L"Graphics", L"HeadlightPitchDeg", 6.0 - c.headlightAimDeg);
    }
    c.ambientOcclusion = ReadDouble(L"Graphics", L"AmbientOcclusion", c.ambientOcclusion);
    c.aoRadius         = ReadDouble(L"Graphics", L"AORadius", c.aoRadius);
    c.sunRays          = ReadDouble(L"Graphics", L"SunRays", c.sunRays);
    c.bloom            = ReadDouble(L"Graphics", L"Bloom", c.bloom);
    c.bloomThreshold   = ReadDouble(L"Graphics", L"BloomThreshold", c.bloomThreshold);
    c.exposure         = ReadDouble(L"Graphics", L"Exposure", c.exposure);
    c.contrast         = ReadDouble(L"Graphics", L"Contrast", c.contrast);
    c.saturation       = ReadDouble(L"Graphics", L"Saturation", c.saturation);
    c.vignette         = ReadDouble(L"Graphics", L"Vignette", c.vignette);
    c.antiAliasing     = ReadInt(L"Graphics", L"AntiAliasing", c.antiAliasing);
    c.sharpen          = ReadDouble(L"Graphics", L"Sharpen", c.sharpen);
    c.anisotropy       = ReadDouble(L"Graphics", L"Anisotropy", c.anisotropy);
    c.trilinear        = ReadInt(L"Graphics", L"Trilinear", c.trilinear) != 0;
    c.textureLodBias   = ReadDouble(L"Graphics", L"TextureLodBias", c.textureLodBias);
    c.dirtDetail       = ReadDouble(L"Graphics", L"DirtDetail", c.dirtDetail);
    c.dirtScale        = ReadDouble(L"Graphics", L"DirtScale", c.dirtScale);
    c.tireMarks        = ReadDouble(L"Graphics", L"TireMarks", c.tireMarks);
    c.tireDirt         = ReadDouble(L"Graphics", L"TireDirt", c.tireDirt);
    c.motionBlur       = ReadDouble(L"Graphics", L"MotionBlur", c.motionBlur);
    c.depthOfField     = ReadDouble(L"Graphics", L"DepthOfField", c.depthOfField);
    c.dofFocus         = ReadDouble(L"Graphics", L"DofFocus", c.dofFocus);
    c.hideHud          = ReadInt(L"Other", L"HideHud", c.hideHud) != 0;
    c.hideCursor       = ReadInt(L"Other", L"HideCursor", c.hideCursor) != 0;
    c.fadeMenu         = ReadInt(L"Other", L"FadeMenu", c.fadeMenu) != 0;
    c.fullscreenWindow = ReadInt(L"Other", L"FullscreenWindow", c.fullscreenWindow) != 0;
    c.dumpShaders      = ReadInt(L"Graphics", L"DumpShaders", c.dumpShaders) != 0;

    // Keep every value inside a range the physics can't blow up with.
    c.viewMatrixIndex      = Clamp(c.viewMatrixIndex, 0, 10000);
    c.snapAngleDeg         = Clamp(c.snapAngleDeg, 1.0, 180.0);
    c.teleportDistance     = Clamp(c.teleportDistance, 0.01, 1e6);
    c.rotationResponse     = Clamp(c.rotationResponse, 0.0, 1000.0);
    c.maxLagDeg            = Clamp(c.maxLagDeg, 0.0, 90.0);
    c.bikeSearchRadius     = Clamp(c.bikeSearchRadius, 0.5, 100.0);
    c.cameraAngleDeg       = Clamp(c.cameraAngleDeg, -45.0, 60.0);
    c.headForward          = Clamp(c.headForward, -5.0, 5.0);
    c.headHeight           = Clamp(c.headHeight, -5.0, 5.0);
    c.horizonLock          = Clamp(c.horizonLock, 0.0, 1.0);
    c.pitchLock            = Clamp(c.pitchLock, 0.0, 1.0);
    c.maxRollCorrectionDeg = Clamp(c.maxRollCorrectionDeg, 0.0, 180.0);
    c.steadiness           = Clamp(c.steadiness, 0.0, 2.0);
    c.airLooseness         = Clamp(c.airLooseness, 0.0, 1.0);
    c.whipLock             = Clamp(c.whipLock, 0.0, 1.0);
    c.lookSideDeg          = Clamp(c.lookSideDeg, 10.0, 90.0);
    c.lookTurnSpeed        = Clamp(c.lookTurnSpeed, 2.0, 40.0);
    c.apexLook             = Clamp(c.apexLook, 0.0, 1.0);
    c.apexSpeed            = Clamp(c.apexSpeed, 0.5, 5.0);
    c.apexAnticipation     = 0.0;  // work in progress: off for now, whatever the ini says
    c.lookSideForward      = Clamp(c.lookSideForward, -5.0, 5.0);
    c.lookSideHeight       = Clamp(c.lookSideHeight, -5.0, 5.0);
    c.lookBackForward      = Clamp(c.lookBackForward, -5.0, 5.0);
    c.lookBackHeight       = Clamp(c.lookBackHeight, -5.0, 5.0);
    c.verticalResponse     = Clamp(c.verticalResponse, 0.0, 1000.0);
    c.maxVerticalLag       = Clamp(c.maxVerticalLag, 0.0, 10.0);
    c.compressionPerG      = Clamp(c.compressionPerG, 0.0, 1.0);
    c.springHz             = Clamp(c.springHz, 0.0, 50.0);
    c.dampingRatio         = Clamp(c.dampingRatio, 0.0, 10.0);
    c.maxCompression       = Clamp(c.maxCompression, 0.0, 10.0);
    c.maxExtension         = Clamp(c.maxExtension, 0.0, 10.0);
    c.nodDegPerMeter       = Clamp(c.nodDegPerMeter, -1000.0, 1000.0);
    c.accelFilterHz        = Clamp(c.accelFilterHz, 0.0, 200.0);
    c.replayDistance       = Clamp(c.replayDistance, 0.5, 30.0);
    c.replayHeight         = Clamp(c.replayHeight, -2.0, 15.0);
    c.replayPitchDeg       = Clamp(c.replayPitchDeg, -45.0, 80.0);
    c.replayCamera         = Clamp(c.replayCamera, 0, 2);
    c.helmetHeight         = Clamp(c.helmetHeight, -2.0, 5.0);
    c.helmetForward        = Clamp(c.helmetForward, -3.0, 3.0);
    c.helmetTiltDeg        = Clamp(c.helmetTiltDeg, -30.0, 80.0);
    c.helmetMount          = Clamp(c.helmetMount, 0, 3);
    c.sideHeight           = Clamp(c.sideHeight, -2.0, 5.0);
    c.sideForward          = Clamp(c.sideForward, -3.0, 3.0);
    c.sideTiltDeg          = Clamp(c.sideTiltDeg, -30.0, 80.0);
    c.sideOut              = Clamp(c.sideOut, 0.0, 3.0);
    c.chestHeight          = Clamp(c.chestHeight, -3.0, 3.0);
    c.chestForward         = Clamp(c.chestForward, -3.0, 3.0);
    c.chestTiltDeg         = Clamp(c.chestTiltDeg, -30.0, 80.0);
    c.helmetSmoothing      = Clamp(c.helmetSmoothing, 0.0, 1.0);
    c.helmetLevel          = Clamp(c.helmetLevel, 0.0, 1.0);
    if (c.timeOfDay >= 0.0)
        c.timeOfDay        = std::fmod(c.timeOfDay, 24.0);
    else
        c.timeOfDay        = -1.0;
    c.nightBrightness      = Clamp(c.nightBrightness, 0.0, 2.0);
    c.shadows              = Clamp(c.shadows, 0.0, 1.0);
    c.shadowLength         = Clamp(c.shadowLength, 1.0, 30.0);
    c.shadowDistance       = Clamp(c.shadowDistance, 5.0, 200.0);
    c.shadowResolution     = c.shadowResolution < 0 ? 0 : c.shadowResolution > 2 ? 2 : c.shadowResolution;
    c.shadowMapSize        = 2048 << c.shadowResolution;
    c.headlight            = Clamp(c.headlight, 0.0, 1.0);
    c.headlightBeamDeg     = Clamp(c.headlightBeamDeg, 5.0, 90.0);
    c.headlightForward     = Clamp(c.headlightForward, -3.0, 3.0);
    c.headlightHeight      = Clamp(c.headlightHeight, -3.0, 3.0);
    c.headlightAimDeg      = Clamp(c.headlightAimDeg, -20.0, 15.0);
    c.ambientOcclusion     = Clamp(c.ambientOcclusion, 0.0, 1.0);
    c.aoRadius             = Clamp(c.aoRadius, 0.1, 5.0);
    c.sunRays              = Clamp(c.sunRays, 0.0, 1.0);
    c.bloom                = Clamp(c.bloom, 0.0, 1.0);
    c.bloomThreshold       = Clamp(c.bloomThreshold, 0.0, 1.0);
    c.exposure             = Clamp(c.exposure, -3.0, 3.0);
    c.contrast             = Clamp(c.contrast, -0.5, 0.5);
    c.saturation           = Clamp(c.saturation, -1.0, 1.0);
    c.vignette             = Clamp(c.vignette, 0.0, 1.0);
    c.antiAliasing         = Clamp(c.antiAliasing, 0, 2);
    c.sharpen              = Clamp(c.sharpen, 0.0, 1.0);
    c.anisotropy           = Clamp(c.anisotropy, 1.0, 16.0);
    c.textureLodBias       = Clamp(c.textureLodBias, -2.0, 1.0);
    c.dirtDetail           = Clamp(c.dirtDetail, 0.0, 1.0);
    c.dirtScale            = Clamp(c.dirtScale, 0.25, 4.0);
    c.tireMarks            = Clamp(c.tireMarks, 0.0, 1.0);
    c.tireDirt             = Clamp(c.tireDirt, 0.0, 1.0);
    c.motionBlur           = Clamp(c.motionBlur, 0.0, 1.0);
    c.depthOfField         = Clamp(c.depthOfField, 0.0, 1.0);
    c.dofFocus             = Clamp(c.dofFocus, 0.0, 500.0);
    return c;
}

}  // namespace

void ReloadConfig()
{
    g_activePreset = 0;
    if (GetFileAttributesW(g_path) == INVALID_FILE_ATTRIBUTES) {
        Log("config: %ls not found, using built-in defaults", g_path);
        g_config = Config{};
        for (Config& preset : g_presets)
            preset = g_config;
        return;
    }
    const Config c = ParseConfig(Config{});  // from the defaults: keys missing from the file keep sane values
    g_config = c;
    // The presets: the active one is what was just read; the others from their own sections (what
    // a section doesn't have, or a preset never saved, is a copy of the active one).
    wchar_t buffer[16];
    GetPrivateProfileStringW(L"Presets", L"Active", L"1", buffer, _countof(buffer), g_path);
    g_activePreset = Clamp(_wtoi(buffer) - 1, 0, kPresetCount - 1);
    for (int i = 0; i < kPresetCount; ++i) {
        g_presets[i] = c;
        if (i == g_activePreset)
            continue;
        g_presetSection = PresetSection(i);
        g_presets[i] = ParseConfig(c);
        g_presetSection = nullptr;
    }
    Log("config: loaded %ls (enabled=%d, %s camera, modelview='%s', camera program has '%s', up=%.2f,%.2f,%.2f)",
        g_path, c.enabled ? 1 : 0, c.customCamera ? "custom" : "game", c.modelviewUniform, c.cameraProgramUniform,
        c.worldUp[0], c.worldUp[1], c.worldUp[2]);
    Log("config: shaders %s (time of day %s%.2f, shadows %.2f, AO %.2f, bloom %.2f, AA %d, %gx anisotropic, dirt %.2f, "
        "motion blur %.2f, DOF %.2f)", c.shaders ? "on" : "off", c.timeOfDay < 0 ? "game " : "", c.timeOfDay < 0 ? 0.0 : c.timeOfDay,
        c.shadows, c.ambientOcclusion, c.bloom, c.antiAliasing, c.anisotropy, c.dirtDetail, c.motionBlur, c.depthOfField);
}

}  // namespace mxcam
