// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// gmtime_r / localtime_r under one name on POSIX and Windows.
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace dect2 {
inline void gmTime(time_t t, struct tm* out) {
#ifdef _WIN32
    gmtime_s(out, &t);
#else
    gmtime_r(&t, out);
#endif
}
inline void localTime(time_t t, struct tm* out) {
#ifdef _WIN32
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}

// ---- wall-clock helpers shared by the decoders (previously a private copy in each) ----

// Seconds since the Unix epoch (system clock, truncated); same value as time(nullptr).
inline int64_t unixNow() { return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
// Fractional seconds since the Unix epoch (system clock).
inline double systemNowSec() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
// Monotonic seconds (steady clock) for measuring intervals; the epoch is arbitrary.
inline double steadyNowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
// "HH:MM:SS UTC" for a Unix time (clock part only).
inline std::string utcClockText(int64_t t) {
    const time_t tt = (time_t)t;
    struct tm g;
    gmTime(tt, &g);
    char b[32];
    snprintf(b, sizeof b, "%02d:%02d:%02d UTC", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}
// ISO 8601 "YYYY-MM-DDTHH:MM:SSZ" for a Unix time (strftime form).
inline std::string utcIsoText(int64_t t) {
    const time_t tt = (time_t)t;
    struct tm g;
    gmTime(tt, &g);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}
} // namespace dect2
