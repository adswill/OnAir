// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The table sizes of the trackers (docs/LIMITS-AUDIT.md). A busy receiver must not hit a wall: the lists are large and old
// entries are expired by age, not by a small count. The caps below only stop a runaway feed from eating memory.
// The age is a user setting per mode family (Settings > Decoding, "Remove after"): setStale() is called by the app.
#pragma once
#include <atomic>
#include <cstddef>

namespace dect2 {
namespace limits {
constexpr size_t kAircraft = 10000;          // aircraft in one tracker table
constexpr size_t kVessels = 10000;           // vessels / AIS stations
constexpr size_t kGroundStations = 4096;     // ground stations
constexpr size_t kMessages = 2000;           // messages in a telemetry message list (copied to the UI, so bounded for cost)
constexpr size_t kLogons = 2000;
constexpr double kAeroStaleSec = 6.0 * 3600; // VDL2 / ACARS / HFDL: an aircraft not heard for this long leaves the table

enum class Family { Vdl2, Acars, Hfdl, Adsb, Ais, Uat, Inmsat, Count };

inline std::atomic<double>* staleTable() {
    static std::atomic<double> t[(int)Family::Count] = {-1, -1, -1, -1, -1, -1, -1};
    return t;
}
// seconds after the last time an entry was heard that it leaves the table: < 0 the mode's own default, 0 never
inline void setStale(Family f, double sec) { staleTable()[(int)f].store(sec < 0 ? -1 : sec); }
// the age limit in force; `modeDefault` (seconds, 0 = never) is the mode's own rule when the user did not choose one
inline double staleSec(Family f, double modeDefault) {
    const double v = staleTable()[(int)f].load();
    const double s = v < 0 ? modeDefault : v;
    return s <= 0 ? 1e18 : s;
}
}  // namespace limits
}  // namespace dect2
