// paths.cpp - see paths.h.
#include "paths.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>

namespace mxcam {
namespace paths {
namespace {

wchar_t g_game[MAX_PATH] = L"";
wchar_t g_data[MAX_PATH] = L"";
wchar_t g_ini[MAX_PATH] = L"";
wchar_t g_personal[MAX_PATH] = L"";
char    g_note[512] = "";

bool Exists(const wchar_t* path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

// "<folder>\" for an environment variable naming a folder (and a subfolder of it, if any).
bool FolderFromEnvironment(const wchar_t* name, const wchar_t* sub, wchar_t out[MAX_PATH])
{
    wchar_t value[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(name, value, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;
    for (wchar_t* c = value; *c; ++c)
        if (*c == L'/')
            *c = L'\\';
    const size_t n = wcslen(value);
    const wchar_t* slash = n > 0 && value[n - 1] == L'\\' ? L"" : L"\\";
    if (sub)
        return swprintf_s(out, MAX_PATH, L"%s%s%s\\", value, slash, sub) > 0;
    return swprintf_s(out, MAX_PATH, L"%s%s", value, slash) > 0;
}

// Whether an existing file can be opened for writing (it isn't changed).
bool CanWriteFile(const wchar_t* path)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(f);
    return true;
}

const wchar_t kIniName[] = L"MXS_REMASTERED.ini";
const wchar_t kOldIniName[] = L"mxcam.ini";

// The mod's files in folder under their old names (mxcam.*, before they were MXS_REMASTERED*): the
// settings and the shader dumps renamed (unless there are new ones already), the old logs and
// reports deleted (new ones are written every time).
void Rename(const wchar_t* folder)
{
    static const wchar_t* const kRenamed[][2] = {
        { L"mxcam.ini",     L"MXS_REMASTERED.ini" },
        { L"mxcam_shaders", L"MXS_REMASTERED_shaders" },
        { L"mxcam_sounds",  L"MXS_REMASTERED_sounds" },
    };
    static const wchar_t* const kDeleted[] = { L"mxcam.log", L"mxcam_trace.csv", L"mxcam_bike.txt" };
    wchar_t from[MAX_PATH], to[MAX_PATH];
    for (const auto& r : kRenamed) {
        swprintf_s(from, L"%s%s", folder, r[0]);
        swprintf_s(to, L"%s%s", folder, r[1]);
        if (Exists(from) && !Exists(to))
            MoveFileW(from, to);
    }
    for (const wchar_t* name : kDeleted) {
        swprintf_s(from, L"%s%s", folder, name);
        if (Exists(from))
            DeleteFileW(from);
    }
}

}  // namespace

bool CanWriteFolder(const wchar_t* folder)
{
    if (!folder || !folder[0])
        return false;
    wchar_t test[MAX_PATH];
    if (swprintf_s(test, L"%sMXS_REMASTERED_write_test_%lu.tmp", folder, GetCurrentProcessId()) <= 0)
        return false;
    HANDLE f = CreateFileW(test, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(f);
    return true;
}

void Init()
{
    // The game folder: the one this DLL was loaded from.
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Init), &self)) {
        const DWORD length = GetModuleFileNameW(self, g_game, MAX_PATH);
        wchar_t* slash = length > 0 && length < MAX_PATH ? wcsrchr(g_game, L'\\') : nullptr;
        if (slash)
            slash[1] = L'\0';
        else
            g_game[0] = L'\0';
    }

    // The game's personal folder (it reads mods from there as well as from its own folder).
    if (!FolderFromEnvironment(L"MX_HOME", nullptr, g_personal) &&
        !FolderFromEnvironment(L"LOCALAPPDATA", L"MX Simulator", g_personal))
        g_personal[0] = L'\0';

    wchar_t user[MAX_PATH] = L"", userIni[MAX_PATH] = L"", gameIni[MAX_PATH] = L"", gameOldIni[MAX_PATH] = L"";
    if (FolderFromEnvironment(L"LOCALAPPDATA", L"MXS Remastered", user))
        swprintf_s(userIni, L"%s%s", user, kIniName);
    if (g_game[0]) {
        swprintf_s(gameIni, L"%s%s", g_game, kIniName);
        swprintf_s(gameOldIni, L"%s%s", g_game, kOldIniName);
    }

    const bool gameWritable = g_game[0] && CanWriteFolder(g_game) && (!Exists(gameIni) || CanWriteFile(gameIni)) &&
                              (!Exists(gameOldIni) || CanWriteFile(gameOldIni));
    // The files under the names they had before (mxcam.*) get the new ones.
    if (gameWritable)
        Rename(g_game);
    if (user[0] && Exists(user))
        Rename(user);
    // (Read from the game folder when it can't be renamed there.)
    if (!Exists(gameIni) && Exists(gameOldIni))
        wcscpy_s(gameIni, gameOldIni);

    if (userIni[0] && Exists(userIni)) {
        // Settings moved there once already: they stay there.
        wcscpy_s(g_data, user);
        snprintf(g_note, sizeof g_note, "files: settings and logs in %ls (moved there earlier)", user);
    } else if (gameWritable || !user[0]) {
        wcscpy_s(g_data, g_game);
        snprintf(g_note, sizeof g_note, "files: settings and logs in the game folder %ls", g_game);
    } else {
        // The game folder needs administrator rights to write to: the user's own folder instead,
        // starting from the settings in the game folder (if any).
        CreateDirectoryW(user, nullptr);
        if (CanWriteFolder(user)) {
            const bool copied = Exists(gameIni) && CopyFileW(gameIni, userIni, TRUE);
            wcscpy_s(g_data, user);
            snprintf(g_note, sizeof g_note,
                     "files: the game folder %ls can't be written to without administrator rights; settings and logs in %ls%s",
                     g_game, user, copied ? " (copied the game folder's settings there)" : "");
        } else {
            wcscpy_s(g_data, g_game);
            snprintf(g_note, sizeof g_note, "files: neither the game folder %ls nor %ls can be written to", g_game, user);
        }
    }
    swprintf_s(g_ini, L"%s%s", g_data, kIniName);
    // (Settings under the old name that couldn't be renamed: read and written as they are.)
    wchar_t old[MAX_PATH];
    swprintf_s(old, L"%s%s", g_data, kOldIniName);
    if (!Exists(g_ini) && Exists(old))
        wcscpy_s(g_ini, old);
}

const wchar_t* GameFolder() { return g_game; }
const wchar_t* DataFolder() { return g_data; }
const wchar_t* IniPath() { return g_ini; }
const wchar_t* PersonalFolder() { return g_personal; }
const char* Note() { return g_note; }

}  // namespace paths
}  // namespace mxcam
