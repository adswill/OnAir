// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GPS interference indicator from ADS-B accuracy data. Aircraft broadcast how good their own GNSS position is (NACp, NIC); when a jammer
// or spoofer is near, many aircraft in one area report a collapsed NACp / NIC, or their position jumps. This module counts, per 0.5 degree
// grid cell and sliding window, how many aircraft were heard and how many of them looked degraded. It keeps no aircraft ids in its output.
// An indicator only: old transponders, antenna shading and receiver CPR errors give the same symptoms. See docs/gps-jamming-map.md.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dect2 {

struct GpsJamObs {
    uint32_t icao = 0;           // only used in memory, to count one aircraft once per cell; never part of the output
    double lat = 0, lon = 0, timeSec = 0;
    int nacp = -1;               // navigation accuracy category, position (0 - 11), -1 not known
    int nic = -1;                // navigation integrity category (0 - 11) from the airborne position type code, -1 not known
    int sil = -1;                // source integrity level (0 - 3), -1 not known (kept for the detail view; not used in the verdict)
    bool ground = false;         // surface targets are ignored
};

struct GpsJamCell {
    int latIdx = 0, lonIdx = 0;  // floor(lat / 0.5), floor(lon / 0.5)
    int aircraft = 0;            // distinct aircraft heard in the window
    int degraded = 0;            // of them: low NACp / NIC, or a position jump, at least once in the window
    int jumps = 0;               // of the degraded ones: how many had a position jump
    int level() const;           // 0 normal, 1 some, 2 many, 3 most; -1 when too few aircraft
    double south() const { return latIdx * 0.5; }
    double west() const { return lonIdx * 0.5; }
};

constexpr double kGpsJamCellDeg = 0.5;
constexpr int kGpsJamMinAircraft = 3;
constexpr double kGpsJamWindowSec = 3600;
constexpr double kGpsJamJumpKt = 2000;      // two fixes that imply a faster ground speed than this are one bad fix, not a flight
constexpr int kGpsJamMaxCells = 600;        // what the map server accepts in one post

class GpsJamGrid {
public:
    explicit GpsJamGrid(double windowSec = kGpsJamWindowSec) : window_(windowSec) {}
    // NACp below 7 (error over 0.1 nm) or NIC below 6 (containment over 1 nm): a normal GNSS receiver in a modern aircraft reports better
    static bool degradedAccuracy(int nacp, int nic);
    // Position fixes of one aircraft that imply more than kGpsJamJumpKt
    static bool impliesJump(double lat1, double lon1, double t1, double lat2, double lon2, double t2);
    void observe(const GpsJamObs& o);
    // Cells with at least minAircraft aircraft in the window ending at now (sorted by cell). Entries older than the window are dropped.
    std::vector<GpsJamCell> cells(double now, int minAircraft = kGpsJamMinAircraft);
    void clear();
    size_t trackedAircraft() const { return last_.size(); }
    // The body for the map server's POST /jam: aggregate cells only, no ids. Empty string when there is nothing to send.
    static std::string toJson(const std::string& station, const std::vector<GpsJamCell>& cells);

private:
    struct Entry { double t = 0; bool bad = false, jump = false; };
    struct Last { double lat = 0, lon = 0, t = 0; };
    double window_;
    std::unordered_map<int64_t, std::unordered_map<uint32_t, Entry>> cell_;   // cell key -> icao -> verdict
    std::unordered_map<uint32_t, Last> last_;
};

} // namespace dect2
