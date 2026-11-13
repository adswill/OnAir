// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One clock for all modes (#218): where the times in the logs and exports come from.
//   Computer: the computer's clock, read when the decoder produces the result. Lost samples and Stop / Start cannot move it
//             (the signal time of the stream stops while the radio is stopped or drops blocks; the computer clock does not).
//   Signal:   times counted from the start of the stream (a decoded file or the time machine gives the same times on every run).
// A decoder stamps each result with stamp() when it is made and the screens and exports turn it into text with epoch() / iso8601().
// Pure functions take every input explicitly so tests need no clock.
#pragma once
#include "time_fmt.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace dect2::timesrc {

enum class Source { Computer = 0, Signal = 1 };

inline std::atomic<int>& sourceVar() { static std::atomic<int> v{0}; return v; }
inline Source source() { return sourceVar().load() == 1 ? Source::Signal : Source::Computer; }
inline void setSource(Source s) { sourceVar().store(s == Source::Signal ? 1 : 0); }

// The computer's clock now: UTC seconds since 1970 with milliseconds.
inline double wallNow() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// What a decoder stores in a result when it is made: the computer clock, or 0 ("not stamped") when times follow the signal.
inline double stamp(Source s) { return s == Source::Computer ? wallNow() : 0.0; }
inline double stamp() { return stamp(source()); }

// The clock time of a result. stored: its stamp (0: none); signalSec: its signal time (< 0: unknown); nowSignal and nowWall: the
// stream's signal time and the computer's clock at one moment (the receiver now). 0 when the time is not known.
inline double epoch(Source s, double stored, double signalSec, double nowSignal, double nowWall) {
    if (s == Source::Computer && stored > 0) return stored;
    if (signalSec < 0) return 0.0;
    return nowWall - (nowSignal - signalSec);
}

// Seconds since 1970 of broken-down fields read as UTC (days-from-civil), used to find the local zone's offset.
inline long long civilSecs(const struct tm& g) {
    long long y = g.tm_year + 1900, m = g.tm_mon + 1;
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + g.tm_mday - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + g.tm_hour * 3600LL + g.tm_min * 60LL + g.tm_sec;
}

// "2026-10-25T14:03:07.123+00:00" (ISO 8601 with milliseconds and the offset, UTC or the computer's local zone as chosen in View).
inline std::string iso8601(double epochSec, timefmt::Zone z) {
    if (epochSec <= 0) return "";
    double fl = std::floor(epochSec);
    int ms = (int)std::lround((epochSec - fl) * 1000.0);
    if (ms >= 1000) { ms -= 1000; fl += 1; }
    const time_t tt = (time_t)fl;
    struct tm g;
    int offMin = 0;
    if (z == timefmt::Zone::Local) {
        localTime(tt, &g);
        offMin = (int)((civilSecs(g) - (long long)tt) / 60);   // the local fields read as UTC, minus the true UTC time
    } else gmTime(tt, &g);
    char b[64];
    const int a = offMin < 0 ? -offMin : offMin;
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02d:%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, ms,
             offMin < 0 ? '-' : '+', a / 60, a % 60);
    return b;
}
inline std::string iso8601(double epochSec) { return iso8601(epochSec, timefmt::zone()); }

} // namespace dect2::timesrc
