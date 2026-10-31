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

} // namespace dect2
