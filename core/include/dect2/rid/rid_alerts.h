// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Drone airspace alerts (3.9, docs/SMART5-avia-3.9.md) for Remote ID and DJI DroneID: a drone flying higher than the open-category
// limit (120 m above the take-off point or the ground, EU 2019/947 and FAA part 107 both use 120 m / 400 ft), a drone inside a radius of
// a known airport, and a drone near a manned aircraft that this receiver hears on ADS-B at the same time. It is the drone that is
// reported (its broadcast ID and position); the operator position in the broadcast is not used. Pure logic, no clock.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct DroneFix {
    std::string id;              // broadcast serial / session ID as decoded
    double t = 0, lat = 0, lon = 0;
    double heightM = -1000;      // above take-off or ground; <= -1000 unknown
    double altM = -1000;         // geodetic or barometric altitude (m); <= -1000 unknown
};

struct MannedFix {               // an aircraft from ADS-B (or FLARM)
    uint32_t icao = 0;
    std::string callsign;
    double t = 0, lat = 0, lon = 0;
    double altM = -1000;         // altitude in m (barometric is fine near the ground)
};

struct DroneAlert {
    std::string kind;            // "high", "airport", "aircraft"
    std::string id;
    double t = 0, lat = 0, lon = 0;
    double value = 0;            // m above take-off, km to the airport, m to the aircraft
    std::string other;           // the airport code or the aircraft callsign / address
    std::string text;
};

class DroneAirspaceWatch {
public:
    double maxHeightM = 120;
    double airportKm = 5;
    double nearHorizM = 1000, nearVertM = 300;
    double aircraftMaxAgeSec = 10;
    double cooldownSec = 600;
    // nearest airport: code and km, false when none within the search radius
    std::function<bool(double lat, double lon, std::string& code, double& km)> nearestAirport;

    void aircraft(const MannedFix& a) { air_[a.icao] = a; }

    // call for every drone fix; alerts are appended
    void drone(const DroneFix& d, std::vector<DroneAlert>& out) {
        if (d.heightM > maxHeightM + 5) {   // a little above the line: the reported height is coarse
            char b[160]; snprintf(b, sizeof b, "drone %s at %.0f m above take-off, over the %.0f m limit", d.id.c_str(), d.heightM, maxHeightM);
            raise("high", d, d.heightM, "", b, out);
        }
        std::string code; double km = 0;
        if (nearestAirport && nearestAirport(d.lat, d.lon, code, km) && km < airportKm) {
            char b[160]; snprintf(b, sizeof b, "drone %s %.1f km from %s", d.id.c_str(), km, code.c_str());
            raise("airport", d, km, code, b, out);
        }
        if (d.altM > -1000) {
            const MannedFix* best = nullptr; double bestM = 1e18;
            for (auto it = air_.begin(); it != air_.end();) {
                const MannedFix& a = it->second;
                if (d.t - a.t > 120) { it = air_.erase(it); continue; }
                ++it;
                if (std::fabs(d.t - a.t) > aircraftMaxAgeSec || a.altM <= -1000) continue;
                const double h = distM(d.lat, d.lon, a.lat, a.lon), v = std::fabs(d.altM - a.altM);
                if (h < nearHorizM && v < nearVertM && h < bestM) { bestM = h; best = &a; }
            }
            if (best) {
                const std::string who = best->callsign.empty() ? hex(best->icao) : best->callsign;
                char b[200]; snprintf(b, sizeof b, "drone %s %.0f m from aircraft %s (%.0f m height difference)", d.id.c_str(), bestM, who.c_str(), std::fabs(d.altM - best->altM));
                raise("aircraft", d, bestM, who, b, out);
            }
        }
    }
    void clear() { air_.clear(); last_.clear(); }
    static double distM(double la1, double lo1, double la2, double lo2) {
        const double r = M_PI / 180, x = (lo2 - lo1) * r * std::cos((la1 + la2) * 0.5 * r), y = (la2 - la1) * r;
        return 6371000.0 * std::hypot(x, y);
    }

private:
    std::map<uint32_t, MannedFix> air_;
    std::map<std::string, double> last_;
    static std::string hex(uint32_t v) { char b[12]; snprintf(b, sizeof b, "%06X", v & 0xFFFFFF); return b; }
    void raise(const char* kind, const DroneFix& d, double value, const std::string& other, const char* text, std::vector<DroneAlert>& out) {
        const std::string key = std::string(kind) + "|" + d.id + "|" + other;
        auto it = last_.find(key);
        if (it != last_.end() && d.t - it->second < cooldownSec) return;
        last_[key] = d.t;
        DroneAlert a; a.kind = kind; a.id = d.id; a.t = d.t; a.lat = d.lat; a.lon = d.lon; a.value = value; a.other = other; a.text = text;
        out.push_back(a);
    }
};

} // namespace dect2
