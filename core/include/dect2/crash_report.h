// Crash reports: when the program dies, a short text file (and on Windows a minidump) says where, so a user has something to send.
#pragma once
#include <string>

namespace dect2::crash {

// The folder for the log and the crash report: %APPDATA%\OnAir on Windows, ~/Library/Logs/OnAir on macOS, ~/.config/onair elsewhere.
std::string logDir();

// Install the crash handler. Call it first thing in main. The report goes to <dir>/<name>.txt (Windows: and <name>.dmp). dir defaults to logDir().
// It also checks whether the run before this one crashed (see lastRunCrashed) and then marks this start.
void install(const char* name = "crash", const char* dir = nullptr);

// True when the previous run wrote a crash report; reportPath is that file. Only meaningful after install().
bool lastRunCrashed(std::string& reportPath);

// Keep the previous log: <dir>/onair.log becomes <dir>/onair-prev.log (one old log is kept). Nothing happens when there is no log.
void rotateLog(const std::string& dir);

#ifdef _WIN32
// Windows has no console: rotate the old log and send standard error to a new <logDir>/onair.log.
void startLog();
#endif

}
