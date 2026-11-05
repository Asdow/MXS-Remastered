// preset_test.cpp
//   The settings presets (config.cpp): switching keeps each preset's settings apart, SAVE writes all
//   three, and loading the ini brings back the one in use and the other two as saved. An ini from
//   before presets loads with all three as its settings.
//
//   Build from a Developer Command Prompt:
//     cl /nologo /EHsc /std:c++17 /W4 tests\preset_test.cpp src\config.cpp src\log.cpp user32.lib
//   Run:
//     preset_test.exe

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <string>

#include "../src/config.h"

using namespace mxcam;

namespace {

int g_failures = 0;
void Check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

bool Same(double a, double b) { return std::fabs(a - b) < 1e-6; }

}  // namespace

int main()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring ini = std::wstring(temp) + L"mxcam_preset_test.ini";

    printf("preset test\n");
    // An ini from before presets: a few settings of its own, no [Presets].
    {
        FILE* out = nullptr;
        if (_wfopen_s(&out, ini.c_str(), L"wb") != 0 || !out)
            return 1;
        fputs("[General]\nMenuKey=0x45\n[Leveling]\nHorizonLock=0.25\n[Graphics]\nBloom=0.9\nShadows=0.4\n", out);
        fclose(out);
    }
    LoadConfig(ini.c_str());
    Check(ActivePreset() == 0, "an ini from before presets starts on preset 1");
    Check(Same(GetConfig().horizonLock, 0.25) && Same(GetConfig().bloom, 0.9), "its settings are preset 1's");
    SwitchPreset(1);
    Check(ActivePreset() == 1, "switched to preset 2");
    Check(Same(GetConfig().horizonLock, 0.25) && Same(GetConfig().bloom, 0.9),
          "a preset never saved starts as a copy of the settings in use");

    // Preset 2: its own settings.
    EditConfig().bloom = 0.1;
    EditConfig().hideHud = true;
    EditConfig().menuKey = 0x50;  // a keybind: not part of the presets
    SwitchPreset(0);
    Check(Same(GetConfig().bloom, 0.9) && !GetConfig().hideHud, "back on preset 1: its settings, not preset 2's");
    Check(GetConfig().menuKey == 0x50, "keybinds stay the same across presets");
    SwitchPreset(1);
    Check(Same(GetConfig().bloom, 0.1) && GetConfig().hideHud, "preset 2 kept its changes");

    // Preset 3 differs too; save on preset 2.
    SwitchPreset(2);
    EditConfig().shadows = 1.0;
    EditConfig().customCamera = true;
    SwitchPreset(1);
    Check(SaveConfig(), "saved");

    // Reload: on preset 2, the others as they were.
    EditConfig().bloom = 0.77;  // unsaved: lost on reload
    ReloadConfig();
    Check(ActivePreset() == 1, "the game starts on the preset in use when saved");
    Check(Same(GetConfig().bloom, 0.1) && GetConfig().hideHud && GetConfig().menuKey == 0x50,
          "preset 2's settings, as saved");
    SwitchPreset(0);
    Check(Same(GetConfig().bloom, 0.9) && Same(GetConfig().shadows, 0.4) && Same(GetConfig().horizonLock, 0.25) &&
              !GetConfig().hideHud,
          "preset 1's settings, as saved");
    SwitchPreset(2);
    Check(Same(GetConfig().shadows, 1.0) && GetConfig().customCamera && Same(GetConfig().bloom, 0.9),
          "preset 3's settings, as saved");

    // DEFAULTS only touches the preset in use.
    ResetTunables();
    Check(Same(GetConfig().shadows, Config{}.shadows) && !GetConfig().customCamera, "DEFAULTS resets the preset in use");
    SwitchPreset(1);
    Check(Same(GetConfig().bloom, 0.1), "and leaves the other presets alone");

    DeleteFileW(ini.c_str());
    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All checks passed\n");
    return 0;
}
