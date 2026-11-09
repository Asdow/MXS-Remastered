// paths.h
//   Where the mod's files go. The game folder (where mx.exe and opengl32.dll are) when the mod can
//   write there; when it can't (the game installed under Program Files, which only administrators can
//   write to), %LOCALAPPDATA%\MXS Remastered\ instead, so nobody has to run the game as administrator
//   to save settings. Once settings live there they stay there.
#pragma once

namespace mxcam {
namespace paths {

// Works the folders out (call first; kernel32 only, safe under the loader lock).
void Init();

// Where opengl32.dll (this) and mx.exe are, with a trailing backslash ("" if unknown).
const wchar_t* GameFolder();

// Where MXS_REMASTERED.ini, MXS_REMASTERED.log, the trace and the other reports go, with a trailing
// backslash.
const wchar_t* DataFolder();

// DataFolder() + "MXS_REMASTERED.ini" (or the settings under their old name, mxcam.ini, where they
// couldn't be renamed).
const wchar_t* IniPath();

// The game's personal folder, which it also reads .saf archives from: %LOCALAPPDATA%\MX Simulator\
// (or MX_HOME), with a trailing backslash ("" if unknown). Not created here.
const wchar_t* PersonalFolder();

// Whether files can be created in folder (a test file, deleted at once).
bool CanWriteFolder(const wchar_t* folder);

// What Init decided, for the log once it's open.
const char* Note();

}  // namespace paths
}  // namespace mxcam
