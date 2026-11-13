// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One place that turns an epoch time into text for the screens and exports: UTC or the computer's local time, chosen by the
// global "Time display" setting (View menu). Stored data stays UTC; only the text shown/exported changes. Pure functions
// (format) take the zone explicitly so tests do not depend on the machine's time zone or on the global setting.
#pragma once
#include "timecompat.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace dect2::timefmt {

enum class Zone { Utc = 0, Local = 1 };
enum class Style {
    Clock,        // "HH:MM:SS"
    ClockShort,   // "HH:MM"
    DateClock,    // "YYYY-MM-DD HH:MM:SS"
    DateClockMin  // "YYYY-MM-DD HH:MM"
};

inline std::atomic<int>& zoneVar() { static std::atomic<int> v{0}; return v; }
inline std::atomic<bool>& labelVar() { static std::atomic<bool> v{false}; return v; }
inline Zone zone() { return zoneVar().load() == 1 ? Zone::Local : Zone::Utc; }
inline void setZone(Zone z) { zoneVar().store(z == Zone::Local ? 1 : 0); }
inline bool label() { return labelVar().load(); }
inline void setLabel(bool on) { labelVar().store(on); }
inline const char* zoneName(Zone z) { return z == Zone::Local ? "local" : "UTC"; }

// Text for an epoch time in the given zone. t <= 0 gives "-" (not set). withLabel appends " (UTC)" or " (local)".
inline std::string format(int64_t t, Zone z, Style s, bool withLabel) {
    if (t <= 0) return "-";
    struct tm g;
    const time_t tt = (time_t)t;
    if (z == Zone::Local) localTime(tt, &g); else gmTime(tt, &g);
    char b[48];
    switch (s) {
    case Style::Clock: snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec); break;
    case Style::ClockShort: snprintf(b, sizeof b, "%02d:%02d", g.tm_hour, g.tm_min); break;
    case Style::DateClock: snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec); break;
    case Style::DateClockMin: snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min); break;
    }
    std::string o = b;
    if (withLabel) { o += " ("; o += zoneName(z); o += ")"; }
    return o;
}

// Same, using the global setting (zone and label).
inline std::string show(int64_t t, Style s = Style::Clock) { return format(t, zone(), s, label()); }
// The amateur-radio convention: always UTC, always labelled (the Ham log).
inline std::string showUtc(int64_t t, Style s = Style::DateClock) { return format(t, Zone::Utc, s, true); }
// "HH:MM:SS" in the global zone, no label (list rows and status bars).
inline std::string clock(int64_t t) { return format(t, zone(), Style::Clock, false); }

// Words for column headings and text next to a time, following the global zone.
inline const char* headTime() { return zone() == Zone::Local ? "Time (local)" : "Time UTC"; }
inline const char* headZone() { return zone() == Zone::Local ? "Local" : "UTC"; }
inline const char* headComputer() { return zone() == Zone::Local ? "Computer (local)" : "Computer UTC"; }
inline const char* sfx() { return zone() == Zone::Local ? " (local)" : " UTC"; }

} // namespace dect2::timefmt
