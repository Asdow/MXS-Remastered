// sound_packs.cpp - see sound_packs.h.
#include "sound_packs.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cwchar>

#include "log.h"

namespace mxcam {
namespace soundpacks {
namespace {

struct Pack {
    const char*    engine;  // the stock soundtable it replaces
    const wchar_t* file;    // mxcam_sounds\<file>.pack, installed as <game folder>\<file>.saf
    const char*    label;
};
// (Named so they sort before the game's own archives (zzz*.saf): the first one that has a file is the
// one read, the way the game's own patches over the years are named.)
const Pack kPacks[kCount] = {
    { "default125",  L"mxsr_engine_default125",  "125 two-stroke" },
    { "default250",  L"mxsr_engine_default250",  "250 two-stroke" },
    { "default250f", L"mxsr_engine_default250f", "250 four-stroke" },
    { "default450f", L"mxsr_engine_default450f", "450 four-stroke" },
};

bool g_installed[kCount] = {};
char g_status[160] = "";

// The folder opengl32.dll (this) is in: the game's.
bool GameFolder(wchar_t* out, size_t size)
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&GameFolder), &self))
        return false;
    const DWORD length = GetModuleFileNameW(self, out, static_cast<DWORD>(size));
    if (length == 0 || length >= size)
        return false;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash)
        return false;
    slash[1] = L'\0';
    return true;
}

bool Paths(int i, wchar_t source[MAX_PATH], wchar_t installed[MAX_PATH])
{
    wchar_t folder[MAX_PATH];
    if (!GameFolder(folder, MAX_PATH))
        return false;
    swprintf_s(source, MAX_PATH, L"%smxcam_sounds\\%s.pack", folder, kPacks[i].file);
    swprintf_s(installed, MAX_PATH, L"%s%s.saf", folder, kPacks[i].file);
    return true;
}

bool Exists(const wchar_t* path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

bool SameFile(const wchar_t* a, const wchar_t* b)
{
    WIN32_FILE_ATTRIBUTE_DATA x, y;
    if (!GetFileAttributesExW(a, GetFileExInfoStandard, &x) || !GetFileAttributesExW(b, GetFileExInfoStandard, &y))
        return false;
    return x.nFileSizeHigh == y.nFileSizeHigh && x.nFileSizeLow == y.nFileSizeLow &&
           CompareFileTime(&x.ftLastWriteTime, &y.ftLastWriteTime) == 0;
}

// A removal that failed while the game had the archive open: done next time instead.
wchar_t g_pendingIni[MAX_PATH] = L"";
void RememberRemoval(int i, bool pending)
{
    wchar_t folder[MAX_PATH];
    if (!g_pendingIni[0] && GameFolder(folder, MAX_PATH))
        swprintf_s(g_pendingIni, L"%smxcam.ini", folder);
    wchar_t key[64];
    swprintf_s(key, L"Remove_%hs", kPacks[i].engine);
    WritePrivateProfileStringW(L"SoundMods", key, pending ? L"1" : nullptr, g_pendingIni);
}
bool RemovalPending(int i)
{
    wchar_t folder[MAX_PATH];
    if (!g_pendingIni[0] && GameFolder(folder, MAX_PATH))
        swprintf_s(g_pendingIni, L"%smxcam.ini", folder);
    wchar_t key[64];
    swprintf_s(key, L"Remove_%hs", kPacks[i].engine);
    return GetPrivateProfileIntW(L"SoundMods", key, 0, g_pendingIni) != 0;
}

}  // namespace

const char* Engine(int i) { return i >= 0 && i < kCount ? kPacks[i].engine : ""; }
const char* Label(int i) { return i >= 0 && i < kCount ? kPacks[i].label : ""; }
bool Installed(int i) { return i >= 0 && i < kCount && g_installed[i]; }

void Startup()
{
    for (int i = 0; i < kCount; ++i) {
        wchar_t source[MAX_PATH], installed[MAX_PATH];
        if (!Paths(i, source, installed))
            continue;
        if (RemovalPending(i)) {
            if (!Exists(installed) || DeleteFileW(installed)) {
                RememberRemoval(i, false);
                Log("sound mods: removed the %s engine sound (it was in use when you turned it off)", kPacks[i].label);
            }
        }
        g_installed[i] = Exists(installed) && !RemovalPending(i);
        // A new version of the mod: its pack copied over the one installed.
        if (g_installed[i] && Exists(source) && !SameFile(source, installed) && CopyFileW(source, installed, FALSE))
            Log("sound mods: updated the %s engine sound to this version's", kPacks[i].label);
        if (g_installed[i])
            Log("sound mods: the %s engine sound is installed (%ls.saf)", kPacks[i].label, kPacks[i].file);
    }
}

const char* Set(int i, bool on)
{
    if (i < 0 || i >= kCount)
        return "";
    wchar_t source[MAX_PATH], installed[MAX_PATH];
    if (!Paths(i, source, installed)) {
        snprintf(g_status, sizeof g_status, "COULD NOT FIND THE GAME FOLDER");
        return g_status;
    }
    if (on) {
        if (!Exists(source)) {
            snprintf(g_status, sizeof g_status, "NOT FOUND: PUT THE MXCAM_SOUNDS FOLDER NEXT TO OPENGL32.DLL");
            Log("sound mods: %ls not found", source);
            return g_status;
        }
        RememberRemoval(i, false);
        if (!CopyFileW(source, installed, FALSE)) {
            snprintf(g_status, sizeof g_status, "COULD NOT INSTALL THE %s SOUND (ERROR %lu)", kPacks[i].label, GetLastError());
            Log("sound mods: could not copy %ls to %ls (error %lu)", source, installed, GetLastError());
            return g_status;
        }
        g_installed[i] = true;
        snprintf(g_status, sizeof g_status, "%s SOUND ON - RESTART THE GAME TO HEAR IT", kPacks[i].label);
        Log("sound mods: installed the %s engine sound (%ls)", kPacks[i].label, installed);
    } else {
        g_installed[i] = false;
        if (Exists(installed) && !DeleteFileW(installed)) {
            // (The game keeps the archives it read open: it goes next time the game starts.)
            RememberRemoval(i, true);
            snprintf(g_status, sizeof g_status, "%s SOUND OFF - GONE NEXT TIME THE GAME STARTS", kPacks[i].label);
            Log("sound mods: the %s engine sound is in use; removed next time the game starts", kPacks[i].label);
        } else {
            snprintf(g_status, sizeof g_status, "%s SOUND OFF - RESTART THE GAME FOR THE STOCK SOUND", kPacks[i].label);
            Log("sound mods: removed the %s engine sound", kPacks[i].label);
        }
    }
    for (char* c = g_status; *c; ++c)
        *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));
    return g_status;
}

}  // namespace soundpacks
}  // namespace mxcam
