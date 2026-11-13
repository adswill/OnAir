// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One CPA or geofence alert as the telemetry carries it (the engine that raises them is in ais_alerts.h).
#pragma once
#include <cstdint>
#include <string>

namespace dect2 {

struct AisAlertRecord {
    double utc = 0;
    std::string kind;            // "cpa", "enter", "exit"
    std::string fence;           // geofence name (enter / exit)
    uint32_t mmsi = 0;
    std::string name;
    double lat = 0, lon = 0, distNm = 0, dcpaNm = 0, tcpaMin = 0;
};

// One anomaly (ais_anomaly.h) as the telemetry carries it.
struct AisAnomalyRecord {
    double utc = 0;
    std::string kind;            // "jump", "clash", "aton_moved", "aton_off_position", "mmsi_class", "speed_type", "name_change", "dark", "reappeared"
    int severity = 1;            // 0 information, 1 warning, 2 alarm
    uint32_t mmsi = 0;
    std::string name;
    double lat = 0, lon = 0;
    std::string text;
};

} // namespace dect2
