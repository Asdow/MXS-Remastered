// log.h
//   Tiny thread-safe text log (MXS_REMASTERED.log, in the folder paths.h picks).
#pragma once

namespace mxcam {

void LogOpen(const wchar_t* path);
void LogClose();

// printf-style. Each call writes one timestamped line.
void Log(const char* format, ...);

// Folder of the log file (with a trailing backslash), for other output files.
const wchar_t* LogDirectory();

}  // namespace mxcam
