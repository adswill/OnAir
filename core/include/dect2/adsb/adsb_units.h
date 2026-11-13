// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Display units for ADS-B numbers: aviation (knots, feet, nautical miles, ft/min) or metric (km/h or m/s, metres, km, m/min).
// The decoder, SBS and the feeds keep the aviation values; only what is shown and exported is converted.
#pragma once
#include <cstdio>
#include <string>

namespace dect2 { namespace adsbu {

enum class System { Aviation = 0, Metric = 1 };
enum class Speed { KmH = 0, MS = 1 };   // the metric speed unit

constexpr double kFtToM = 0.3048, kKtToKmh = 1.852, kKtToMs = 1852.0 / 3600.0, kNmToKm = 1.852, kFpmToMs = 0.3048 / 60.0, kFpmToMmin = 0.3048;

struct Units {
    System sys = System::Aviation;
    Speed spd = Speed::KmH;
    bool metric() const { return sys == System::Metric; }
};

inline double altOut(const Units& u, double ft) { return u.metric() ? ft * kFtToM : ft; }
inline double speedOut(const Units& u, double kt) { return !u.metric() ? kt : u.spd == Speed::KmH ? kt * kKtToKmh : kt * kKtToMs; }
inline double distOut(const Units& u, double nm) { return u.metric() ? nm * kNmToKm : nm; }
inline double vrateOut(const Units& u, double fpm) { return u.metric() ? fpm * kFpmToMmin : fpm; }

inline const char* altUnit(const Units& u) { return u.metric() ? "m" : "ft"; }
inline const char* speedUnit(const Units& u) { return !u.metric() ? "kt" : u.spd == Speed::KmH ? "km/h" : "m/s"; }
inline const char* distUnit(const Units& u) { return u.metric() ? "km" : "nm"; }
inline const char* vrateUnit(const Units& u) { return u.metric() ? "m/min" : "ft/min"; }

// Digits: whole numbers except m/s speeds (one decimal)
inline int speedDecimals(const Units& u) { return u.metric() && u.spd == Speed::MS ? 1 : 0; }
inline int vrateDecimals(const Units&) { return 0; }

inline std::string fmt(double v, int decimals, bool sign = false) {
    char b[48];
    snprintf(b, sizeof b, sign ? "%+.*f" : "%.*f", decimals, v);
    return b;
}

// CSV column names: the unit is in the header
inline std::string altCol(const Units& u) { return u.metric() ? "alt_m" : "alt_ft"; }
inline std::string speedCol(const Units& u) { return !u.metric() ? "speed_kt" : u.spd == Speed::KmH ? "speed_kmh" : "speed_ms"; }
inline std::string vrateCol(const Units& u) { return u.metric() ? "vrate_mmin" : "vrate_fpm"; }
inline std::string distCol(const Units& u) { return u.metric() ? "dist_km" : "dist_nm"; }

}}   // namespace dect2::adsbu
