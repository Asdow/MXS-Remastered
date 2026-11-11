// sound_packs_test.cpp
//   The engine sound packs' install / remove (sound_packs.cpp), in a folder of its own standing in for
//   the game's: the test copies itself there (the packs are found next to the running module), with a
//   MXS_REMASTERED_sounds folder of fake packs, and runs itself again from there - its personal folder
//   (MX_HOME) a stand-in too, so nothing of the real game's is touched.
//
//   Build from a Developer Command Prompt:
//     cl /nologo /EHsc /std:c++17 tests\sound_packs_test.cpp src\sound_packs.cpp src\paths.cpp src\log.cpp user32.lib shell32.lib
#include <windows.h>

#include <cstdio>
#include <string>

#include "../src/log.h"
#include "../src/paths.h"
#include "../src/sound_packs.h"

using namespace mxcam;

namespace {
int g_failures = 0;
void Check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::wstring Read(const std::wstring& p)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f)
        return L"";
    char buffer[256] = {};
    const size_t n = fread(buffer, 1, sizeof buffer - 1, f);
    fclose(f);
    return std::wstring(buffer, buffer + n);
}
void WriteFile(const std::wstring& p, const char* text)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wb") == 0 && f) {
        fputs(text, f);
        fclose(f);
    }
}
}  // namespace

int main(int argc, char** argv)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring folder = self;
    folder = folder.substr(0, folder.find_last_of(L'\\') + 1);
    if (argc < 2 || std::string(argv[1]) != "run") {
        // Set up the stand-in game folder and run there.
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::wstring game = std::wstring(temp) + L"mxcam_sound_packs_test\\";
        const std::wstring home = std::wstring(temp) + L"mxcam_sound_packs_test_home\\";
        CreateDirectoryW(game.c_str(), nullptr);
        CreateDirectoryW(home.c_str(), nullptr);
        CreateDirectoryW((game + L"MXS_REMASTERED_sounds").c_str(), nullptr);
        for (const wchar_t* old : { L"MXS_REMASTERED_engine_default125.saf", L"MXS_REMASTERED_engine_default450f.saf",
                                    L"MXS_REMASTERED_engine_default450f_clear.saf", L"MXS_REMASTERED.ini", L"MXS_REMASTERED.log" })
            DeleteFileW((game + old).c_str());
        WriteFile(game + L"MXS_REMASTERED_sounds\\MXS_REMASTERED_engine_default450f.pack", "450 pack v1");
        WriteFile(game + L"MXS_REMASTERED_sounds\\MXS_REMASTERED_engine_default450f_clear.pack", "450 clear pack v1");
        WriteFile(game + L"MXS_REMASTERED_sounds\\MXS_REMASTERED_engine_default125.pack", "125 pack v1");
        CopyFileW(self, (game + L"sound_packs_test.exe").c_str(), FALSE);
        SetEnvironmentVariableW(L"MX_HOME", home.c_str());  // (the personal folder: a stand-in, not the real one)
        STARTUPINFOW si = { sizeof si };
        PROCESS_INFORMATION pi = {};
        std::wstring command = L"\"" + game + L"sound_packs_test.exe\" run";
        if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, game.c_str(), &si, &pi)) {
            printf("could not run the test in %ls\n", game.c_str());
            return 1;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        return static_cast<int>(code);
    }

    LogOpen((folder + L"MXS_REMASTERED.log").c_str());
    paths::Init();
    const std::wstring saf450 = folder + L"MXS_REMASTERED_engine_default450f.saf", saf125 = folder + L"MXS_REMASTERED_engine_default125.saf";
    const std::wstring saf450clear = folder + L"MXS_REMASTERED_engine_default450f_clear.saf";
    soundpacks::Startup();
    Check(!soundpacks::Installed(3) && !soundpacks::Installed(0), "nothing installed to begin with");

    printf("    %s\n", soundpacks::Set(3, true));
    Check(soundpacks::Installed(3) && Exists(saf450) && Read(saf450) == L"450 pack v1", "turned on: the 450 pack is copied into the game folder as a .saf");
    printf("    %s\n", soundpacks::Set(1, true));
    Check(!soundpacks::Installed(1), "a pack that isn't shipped can't be turned on (and says so)");

    soundpacks::Startup();
    Check(soundpacks::Installed(3), "at the next start it's seen as installed");

    // A new version of the mod: the shipped pack changes, the installed one is brought up to date.
    Sleep(20);
    WriteFile(folder + L"MXS_REMASTERED_sounds\\MXS_REMASTERED_engine_default450f.pack", "450 pack v2");
    soundpacks::Startup();
    Check(Read(saf450) == L"450 pack v2", "a newer shipped pack replaces the installed one at startup");

    printf("    %s\n", soundpacks::Set(3, false));
    Check(!soundpacks::Installed(3) && !Exists(saf450), "turned off: its .saf is removed");

    // The game holds the archives it read open: turning one off then can't remove it yet.
    printf("    %s\n", soundpacks::Set(0, true));
    HANDLE open = CreateFileW(saf125.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    printf("    %s\n", soundpacks::Set(0, false));
    Check(!soundpacks::Installed(0) && Exists(saf125), "turned off while the game has it open: it's still there for now");
    CloseHandle(open);
    soundpacks::Startup();
    Check(!soundpacks::Installed(0) && !Exists(saf125), "and removed the next time the game starts");

    // The 450's two packs (the plain one and the clear one) replace the same stock engine, each its own
    // file: one turned off never touches the other's.
    printf("    %s\n", soundpacks::Set(4, true));
    Check(soundpacks::Installed(4) && Exists(saf450clear) && Read(saf450clear) == L"450 clear pack v1",
          "the clear 450 pack is copied in as a .saf of its own");
    printf("    %s\n", soundpacks::Set(3, true));
    HANDLE plain = CreateFileW(saf450.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    printf("    %s\n", soundpacks::Set(3, false));  // (in use: removed at the next start)
    CloseHandle(plain);
    Check(Exists(saf450clear) && soundpacks::Installed(4), "turning the plain 450 off leaves the clear one installed");
    soundpacks::Startup();
    Check(!Exists(saf450) && Exists(saf450clear) && soundpacks::Installed(4),
          "its removal left for the next start removes only the plain one (each pack has its own note of it)");
    printf("    %s\n", soundpacks::Set(4, false));
    Check(!soundpacks::Installed(4) && !Exists(saf450clear), "and the clear one turns off on its own");

    printf(g_failures ? "%d check(s) FAILED\n" : "All checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
