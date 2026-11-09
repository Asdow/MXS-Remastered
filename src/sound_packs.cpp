// sound_packs.cpp - see sound_packs.h.
#include "sound_packs.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cwchar>

#include "log.h"
#include "paths.h"

namespace mxcam {
namespace soundpacks {
namespace {

struct Pack {
    const char*    engine;  // the stock soundtable it replaces
    const wchar_t* file;    // MXS_REMASTERED_sounds\<file>.pack, installed as <game folder>\<file>.saf
    const wchar_t* oldFile; // what it was called before (mxcam_sounds\mxsr_*)
    const char*    label;
};
// (Named so they sort before the game's own archives (zzz*.saf): the first one that has a file is the
// one read, the way the game's own patches over the years are named.)
const Pack kPacks[kCount] = {
    { "default125",  L"MXS_REMASTERED_engine_default125",  L"mxsr_engine_default125",  "125 two-stroke" },
    { "default250",  L"MXS_REMASTERED_engine_default250",  L"mxsr_engine_default250",  "250 two-stroke" },
    { "default250f", L"MXS_REMASTERED_engine_default250f", L"mxsr_engine_default250f", "250 four-stroke" },
    { "default450f", L"MXS_REMASTERED_engine_default450f", L"mxsr_engine_default450f", "450 four-stroke" },
};

// Where a pack can be installed: the game folder, or - when that needs administrator rights to write
// to (the game installed under Program Files) - the game's personal folder, which it reads archives
// from as well.
enum Place { kGameFolder, kPersonalFolder, kPlaces };
const char* const kPlaceName[kPlaces] = { "game folder", "personal folder" };

bool g_installed[kCount] = {};
char g_status[192] = "";

struct Files {
    wchar_t source[MAX_PATH];             // the pack shipped with the mod
    wchar_t installed[kPlaces][MAX_PATH]; // "" where there's no such folder
};

const wchar_t* FolderOf(Place p) { return p == kGameFolder ? paths::GameFolder() : paths::PersonalFolder(); }

bool Exists(const wchar_t* path)
{
    return path[0] && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// The pack's files. Under their old names (mxcam_sounds\mxsr_*) where they still have them: renamed
// at startup where the folder can be written to, used as they are where it can't.
bool Paths(int i, Files& f)
{
    const wchar_t* game = paths::GameFolder();
    if (!game[0])
        return false;
    static const wchar_t* const kFolders[] = { L"MXS_REMASTERED_sounds", L"mxcam_sounds" };
    const wchar_t* const names[] = { kPacks[i].file, kPacks[i].oldFile };
    f.source[0] = L'\0';
    for (const wchar_t* folder : kFolders) {
        for (const wchar_t* name : names) {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%s%s\\%s.pack", game, folder, name);
            if (!f.source[0] || (!Exists(f.source) && Exists(path)))
                wcscpy_s(f.source, path);
        }
    }
    for (int p = 0; p < kPlaces; ++p) {
        const wchar_t* folder = FolderOf(static_cast<Place>(p));
        f.installed[p][0] = L'\0';
        if (!folder[0])
            continue;
        wchar_t old[MAX_PATH];
        swprintf_s(f.installed[p], L"%s%s.saf", folder, kPacks[i].file);
        swprintf_s(old, L"%s%s.saf", folder, kPacks[i].oldFile);
        if (!Exists(f.installed[p]) && Exists(old))
            wcscpy_s(f.installed[p], old);
    }
    return true;
}

// Pack i under its old names (mxsr_*): the new ones, where the folder can be written to - the copy
// shipped (MXS_REMASTERED_sounds, which paths::Init renamed from mxcam_sounds) and the ones installed.
void RenameOld(int i)
{
    wchar_t from[MAX_PATH], to[MAX_PATH];
    swprintf_s(from, L"%sMXS_REMASTERED_sounds\\%s.pack", paths::GameFolder(), kPacks[i].oldFile);
    swprintf_s(to, L"%sMXS_REMASTERED_sounds\\%s.pack", paths::GameFolder(), kPacks[i].file);
    if (paths::GameFolder()[0] && Exists(from) && !Exists(to))
        MoveFileW(from, to);
    for (int p = 0; p < kPlaces; ++p) {
        const wchar_t* folder = FolderOf(static_cast<Place>(p));
        if (!folder[0])
            continue;
        swprintf_s(from, L"%s%s.saf", folder, kPacks[i].oldFile);
        swprintf_s(to, L"%s%s.saf", folder, kPacks[i].file);
        if (!Exists(from))
            continue;
        if (Exists(to))
            DeleteFileW(from);  // (the same pack twice)
        else if (MoveFileW(from, to))
            Log("sound mods: renamed %ls to %ls", from, to);
    }
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
void RememberRemoval(int i, bool pending)
{
    wchar_t key[64];
    swprintf_s(key, L"Remove_%hs", kPacks[i].engine);
    WritePrivateProfileStringW(L"SoundMods", key, pending ? L"1" : nullptr, paths::IniPath());
}
bool RemovalPending(int i)
{
    wchar_t key[64];
    swprintf_s(key, L"Remove_%hs", kPacks[i].engine);
    return GetPrivateProfileIntW(L"SoundMods", key, 0, paths::IniPath()) != 0;
}

enum class Removed { Gone, InUse, NotAllowed };
// Deletes every installed copy of a pack. blocked: the place of one that needs administrator rights to
// delete (when the result is NotAllowed).
Removed RemoveCopies(const Files& f, int* blocked = nullptr)
{
    Removed result = Removed::Gone;
    for (int p = 0; p < kPlaces; ++p) {
        if (!Exists(f.installed[p]) || DeleteFileW(f.installed[p]))
            continue;
        const DWORD error = GetLastError();
        // (Access denied in a folder that can be written to: the game has the archive open.)
        if (error == ERROR_ACCESS_DENIED && !paths::CanWriteFolder(FolderOf(static_cast<Place>(p)))) {
            Log("sound mods: %ls can't be deleted without administrator rights", f.installed[p]);
            result = Removed::NotAllowed;
            if (blocked)
                *blocked = p;
        } else {
            if (result == Removed::Gone)
                result = Removed::InUse;
            Log("sound mods: %ls is in use (error %lu)", f.installed[p], error);
        }
    }
    return result;
}

void Upper(char* s)
{
    for (char* c = s; *c; ++c)
        *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));
}

}  // namespace

const char* Engine(int i) { return i >= 0 && i < kCount ? kPacks[i].engine : ""; }
const char* Label(int i) { return i >= 0 && i < kCount ? kPacks[i].label : ""; }
bool Installed(int i) { return i >= 0 && i < kCount && g_installed[i]; }

void Startup()
{
    for (int i = 0; i < kCount; ++i) {
        RenameOld(i);
        Files f;
        if (!Paths(i, f))
            continue;
        if (RemovalPending(i)) {
            const Removed r = RemoveCopies(f);
            if (r == Removed::Gone) {
                RememberRemoval(i, false);
                Log("sound mods: removed the %s engine sound (it was in use when you turned it off)", kPacks[i].label);
            } else if (r == Removed::NotAllowed) {
                // (Installed while running as administrator: it can only go the same way.)
                RememberRemoval(i, false);
            }
        }
        bool any = false;
        for (int p = 0; p < kPlaces; ++p) {
            if (!Exists(f.installed[p]))
                continue;
            any = true;
            // A new version of the mod: its pack copied over the one installed.
            if (Exists(f.source) && !SameFile(f.source, f.installed[p]) && CopyFileW(f.source, f.installed[p], FALSE))
                Log("sound mods: updated the %s engine sound to this version's", kPacks[i].label);
            Log("sound mods: the %s engine sound is installed (%ls)", kPacks[i].label, f.installed[p]);
        }
        g_installed[i] = any && !RemovalPending(i);
    }
}

const char* Set(int i, bool on)
{
    if (i < 0 || i >= kCount)
        return "";
    Files f;
    if (!Paths(i, f)) {
        snprintf(g_status, sizeof g_status, "COULD NOT FIND THE GAME FOLDER");
        return g_status;
    }
    if (on) {
        if (!Exists(f.source)) {
            snprintf(g_status, sizeof g_status, "NOT FOUND: PUT THE MXS_REMASTERED_SOUNDS FOLDER NEXT TO OPENGL32.DLL");
            Log("sound mods: %ls not found", f.source);
            return g_status;
        }
        RememberRemoval(i, false);
        // The game folder first; the personal folder when the game folder can't be written to.
        int place = -1;
        DWORD error = 0;
        if (CopyFileW(f.source, f.installed[kGameFolder], FALSE)) {
            place = kGameFolder;
        } else {
            error = GetLastError();
            Log("sound mods: could not copy %ls to %ls (error %lu)", f.source, f.installed[kGameFolder], error);
            if (f.installed[kPersonalFolder][0]) {
                CreateDirectoryW(paths::PersonalFolder(), nullptr);
                if (CopyFileW(f.source, f.installed[kPersonalFolder], FALSE))
                    place = kPersonalFolder;
                else
                    Log("sound mods: could not copy %ls to %ls either (error %lu)", f.source,
                        f.installed[kPersonalFolder], error = GetLastError());
            }
        }
        if (place < 0) {
            snprintf(g_status, sizeof g_status, "COULD NOT INSTALL THE %s SOUND (ERROR %lu)", kPacks[i].label, error);
            Upper(g_status);
            return g_status;
        }
        // (Only one copy: an older one left in the personal folder goes.)
        if (place == kGameFolder && Exists(f.installed[kPersonalFolder]))
            DeleteFileW(f.installed[kPersonalFolder]);
        g_installed[i] = true;
        snprintf(g_status, sizeof g_status, "%s SOUND ON - RESTART THE GAME TO HEAR IT", kPacks[i].label);
        Log("sound mods: installed the %s engine sound in the %s (%ls)", kPacks[i].label, kPlaceName[place],
            f.installed[place]);
    } else {
        int blocked = kGameFolder;
        const Removed r = RemoveCopies(f, &blocked);
        if (r == Removed::NotAllowed) {
            // (Installed while the game ran as administrator, into a folder that needs it - Program
            // Files. The game keeps it open while it runs, so it can only go once the game is closed.)
            g_installed[i] = true;
            const wchar_t* path = f.installed[blocked];
            const wchar_t* name = wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path;
            snprintf(g_status, sizeof g_status, "CLOSE THE GAME, THEN DELETE %ls FROM THE %s", name, kPlaceName[blocked]);
            Log("sound mods: the %s engine sound was installed as administrator; close the game and delete %ls",
                kPacks[i].label, path);
        } else if (r == Removed::InUse) {
            // (The game keeps the archives it read open: it goes next time the game starts.)
            g_installed[i] = false;
            RememberRemoval(i, true);
            snprintf(g_status, sizeof g_status, "%s SOUND OFF - GONE NEXT TIME THE GAME STARTS", kPacks[i].label);
            Log("sound mods: the %s engine sound is in use; removed next time the game starts", kPacks[i].label);
        } else {
            g_installed[i] = false;
            snprintf(g_status, sizeof g_status, "%s SOUND OFF - RESTART THE GAME FOR THE STOCK SOUND", kPacks[i].label);
            Log("sound mods: removed the %s engine sound", kPacks[i].label);
        }
    }
    Upper(g_status);
    return g_status;
}

}  // namespace soundpacks
}  // namespace mxcam
