// preset_test.cpp
//   The settings presets (config.cpp): switching keeps each preset's settings apart, SAVE writes all
//   five, and loading the ini brings back the one in use and the others as saved. An ini from
//   before presets loads with all of them as its settings. Names: kept apart from the settings
//   (DEFAULTS and a pasted code leave them), saved and loaded with the rest.
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

    // Five of them; names of their own.
    Check(kPresetCount == 5, "five presets");
    SwitchPreset(4);
    Check(ActivePreset() == 4, "switched to preset 5");
    EditConfig().bloom = 0.33;
    Check(PresetLabel(4) == "5" && PresetName(4).empty(), "a preset never named shows its number");
    SetPresetName(4, "  Night \"race\" ");
    SetPresetName(0, "My everyday settings for racing");
    Check(PresetName(4) == "Night \"race\"", "a name is kept as typed (spaces at its ends trimmed)");
    Check(static_cast<int>(PresetName(0).size()) == kPresetNameMax, "a name too long is cut to the most allowed");
    SetPresetName(2, std::string("Wet\x01\xE9") + "!");
    Check(PresetName(2) == "Wet!", "characters the menu can't draw are left out");
    ResetTunables();
    Check(PresetName(4) == "Night \"race\"", "DEFAULTS leaves the name alone");
    EditConfig().bloom = 0.33;
    // Someone else's code: their graphics, your name.
    SwitchPreset(1);
    const std::string theirs = PresetCode(1);  // preset 2's graphics (bloom 0.1)
    SwitchPreset(4);
    const PresetCodeResult loaded = LoadPresetCode(("look at mine: " + theirs).c_str());
    Check(loaded.status == PresetCodeResult::Loaded && Same(GetConfig().bloom, 0.1), "a pasted code loads its settings");
    Check(PresetName(4) == "Night \"race\"" && PresetName(1).empty(), "and changes no preset's name");
    Check(SaveConfig(), "saved with names");
    SetPresetName(4, "unsaved");
    ReloadConfig();
    Check(ActivePreset() == 4 && Same(GetConfig().bloom, 0.1), "preset 5 in use, as saved");
    Check(PresetName(4) == "Night \"race\"" && PresetName(0) == "My everyday sett" && PresetName(2) == "Wet!" &&
              PresetName(1).empty() && PresetName(3).empty(),
          "the names come back as saved (quotes and all), the unnamed ones still unnamed");
    SetPresetName(4, "   ");
    Check(PresetName(4).empty() && PresetLabel(4) == "5", "a blank name goes back to the number");
    SaveConfig();
    ReloadConfig();
    Check(PresetName(4).empty() && PresetName(0) == "My everyday sett", "and stays that way once saved");

    DeleteFileW(ini.c_str());
    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All checks passed\n");
    return 0;
}
