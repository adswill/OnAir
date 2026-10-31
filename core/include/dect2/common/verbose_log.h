// "Verbose logging" (View > Verbose logging on the desktop, Settings > Verbose logging on Android): when on, drivers and the engine write extra
// diagnostic lines (USB transfer counts, bytes per second, stall detections and restarts, frame times, thread states) into the log the app
// keeps. Off by default: the lines cost a little CPU and fill the log. One switch for the whole process.
//
// diagLog() is for the lines that always matter (a radio lost and reopened, a stream restarted): they go to the app's log when the app set a
// sink (the engine's log; on Android stderr goes nowhere, so without a sink those lines were never seen), else to stderr.
#pragma once
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>

namespace dect2 {

inline std::atomic<bool>& verboseLoggingFlag() { static std::atomic<bool> on{false}; return on; }
inline bool verboseLogging() { return verboseLoggingFlag().load(std::memory_order_relaxed); }
inline void setVerboseLogging(bool on) { verboseLoggingFlag().store(on); }

inline std::mutex& diagSinkMutex() { static std::mutex m; return m; }
inline std::function<void(const std::string&)>& diagSink() { static std::function<void(const std::string&)> f; return f; }
// the app's log: every diagLog / verboseLog line goes through it (pass an empty function to go back to stderr)
inline void setDiagSink(std::function<void(const std::string&)> f) { std::lock_guard<std::mutex> lk(diagSinkMutex()); diagSink() = std::move(f); }

inline void diagEmit(const std::string& line) {
    std::function<void(const std::string&)> f;
    { std::lock_guard<std::mutex> lk(diagSinkMutex()); f = diagSink(); }
    if (f) { f(line); return; }
    fprintf(stderr, "%s\n", line.c_str());
    fflush(stderr);
}

inline void diagLogV(const char* prefix, const char* fmt, va_list ap) {
    char b[400];
    vsnprintf(b, sizeof b, fmt, ap);
    diagEmit(std::string(prefix) + b);
}
// printf-style, always
inline void diagLog(const char* fmt, ...) { va_list ap; va_start(ap, fmt); diagLogV("", fmt, ap); va_end(ap); }
// printf-style; a line starts with "[v] " so it can be told from the normal lines. Nothing when verbose logging is off.
inline void verboseLog(const char* fmt, ...) {
    if (!verboseLogging()) return;
    va_list ap; va_start(ap, fmt); diagLogV("[v] ", fmt, ap); va_end(ap);
}

} // namespace dect2
