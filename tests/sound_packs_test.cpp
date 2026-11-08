// sound_packs_test.cpp
//   The engine sound packs' install / remove (sound_packs.cpp), in a folder of its own standing in for
//   the game's: the test copies itself there (the packs are found next to the running module), with a
//   mxcam_sounds folder of fake packs, and runs itself again from there.
//
//   Build from a Developer Command Prompt:
//     cl /nologo /EHsc /std:c++17 tests\sound_packs_test.cpp src\sound_packs.cpp src\log.cpp user32.lib
#include <windows.h>

#include <cstdio>
#include <string>

#include "../src/log.h"
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
        CreateDirectoryW(game.c_str(), nullptr);
        CreateDirectoryW((game + L"mxcam_sounds").c_str(), nullptr);
        for (const wchar_t* old : { L"mxsr_engine_default125.saf", L"mxsr_engine_default450f.saf", L"mxcam.ini", L"mxcam.log" })
            DeleteFileW((game + old).c_str());
        WriteFile(game + L"mxcam_sounds\\mxsr_engine_default450f.pack", "450 pack v1");
        WriteFile(game + L"mxcam_sounds\\mxsr_engine_default125.pack", "125 pack v1");
        CopyFileW(self, (game + L"sound_packs_test.exe").c_str(), FALSE);
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

    LogOpen((folder + L"mxcam.log").c_str());
    const std::wstring saf450 = folder + L"mxsr_engine_default450f.saf", saf125 = folder + L"mxsr_engine_default125.saf";
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
    WriteFile(folder + L"mxcam_sounds\\mxsr_engine_default450f.pack", "450 pack v2");
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

    printf(g_failures ? "%d check(s) FAILED\n" : "All checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
