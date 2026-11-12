// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Crash reports: when the program dies, a short text file (and on Windows a minidump) says where, so a user has something to send.
#pragma once
#include <string>

namespace dect2::crash {

// The folder for the log and the crash report: %APPDATA%\OnAir on Windows, ~/Library/Logs/OnAir on macOS, ~/.config/onair elsewhere.
std::string logDir();

// Install the crash handler. Call it first thing in main. The report goes to <dir>/<name>.txt, the stacks of the threads to <name>.dmp
// (Windows; Linux x86-64). dir defaults to logDir().
// It also checks whether the run before this one crashed (see lastRunCrashed) and then marks this start.
void install(const char* name = "crash", const char* dir = nullptr);

// True when the previous run wrote a crash report; reportPath is that file. Only meaningful after install().
bool lastRunCrashed(std::string& reportPath);

// The dump (stacks of all threads, debug_dump.h) of the crash lastRunCrashed() found; empty when there is none.
std::string lastCrashDump();

// Startup guard: install() also counts the previous starts in a row that ended (crash, hang, kill) before startupDone() was called.
// The program can then start with safer settings instead of crashing the same way on every start.
int failedStarts();
// The program is up (its first frames are drawn): clears the count. Only meaningful after install().
void startupDone();
// Keep the previous log: <dir>/onair.log becomes <dir>/onair-prev.log (one old log is kept). Nothing happens when there is no log.
void rotateLog(const std::string& dir);

// Start <logDir>/onair.log for this run (the old one becomes onair-prev.log). Windows has no console, so standard error goes there too;
// elsewhere standard error stays on the terminal and the file holds what appendLog() writes (the program's log), so the log of a run
// that crashed is still there at the next start.
void startLog();
// Add text to onair.log (Windows: through standard error, which is that file). Nothing happens before startLog().
void appendLog(const std::string& text);

}
