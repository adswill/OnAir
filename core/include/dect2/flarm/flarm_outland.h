// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Out-landing alert (3.9, docs/SMART5-avia-3.9.md) for FLARM, OGN and ADS-L traffic: an aircraft that was flying and is now standing still
// on the ground, away from every known airfield, is reported once as a "possible out-landing" with its position. Gliders land in fields
// as a normal part of cross-country flying; the crew then needs a retrieve, and a landing that is not one (a crash) looks the same at
// first. The aircraft's own broadcast is all that is used. Aircraft that ask not to be tracked never reach this (the receivers drop them).
// Pure logic, no clock.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dect2 {

struct TrafficFix {
    uint32_t id = 0;
    double t = 0;                // seconds
    double lat = 0, lon = 0;
    double altM = 0;             // any reference: only changes are used
    double speedMs = 0;          // ground speed
    double climbMs = 0;
    bool reportsGround = false;  // the aircraft itself says it is on the ground
};

struct OutlandingAlert {
    uint32_t id = 0;
    double t = 0, lat = 0, lon = 0;
    double airfieldKm = -1;      // to the nearest known airfield, -1 when none is known within the search
    double flownSec = 0;         // how long it was seen flying before
    std::string text;
};

class OutlandingWatch {
public:
    double flyingSpeedMs = 15;   // faster than this is flying (a glider's slowest is about 18 m/s)
    double minFlyingSec = 120;   // seen flying for at least this long
    double stillSpeedMs = 2.5;   // slower than this is standing
    double stillSec = 60;        // for this long, inside stillRadiusM
    double stillRadiusM = 150;
    double airfieldKm = 3;       // nearer than this to an airfield is a normal landing
    double forgetSec = 3600;
    // the distance to the nearest airfield (km, < 0 when none is known nearby); set by the app from the airport table
    std::function<double(double lat, double lon)> nearestAirfieldKm;

    // call for every position; returns true and fills a when an out-landing is decided (once per landing)
    bool add(const TrafficFix& f, OutlandingAlert* a = nullptr) {
        St& s = st_[f.id];
        if (s.lastT > 0 && f.t - s.lastT > forgetSec) s = St();
        s.lastT = f.t;
        const bool flying = f.speedMs > flyingSpeedMs && !f.reportsGround;
        if (flying) {
            if (s.flyStart < 0 || f.t - s.flyLast > 300) s.flyStart = f.t;   // a long gap starts a new flight
            s.flyLast = f.t; s.stillStart = -1; s.reported = false;
            return false;
        }
        if (f.speedMs > stillSpeedMs || std::fabs(f.climbMs) > 1.0) { s.stillStart = -1; return false; }
        if (s.stillStart < 0 || distM(f.lat, f.lon, s.stillLat, s.stillLon) > stillRadiusM) { s.stillStart = f.t; s.stillLat = f.lat; s.stillLon = f.lon; return false; }
        if (s.reported || s.flyStart < 0 || s.flyLast - s.flyStart < minFlyingSec) return false;
        if (f.t - s.stillStart < stillSec) return false;
        if (s.stillStart - s.flyLast > 600) return false;   // it stopped long after the flight ended: not this landing
        s.reported = true;
        const double km = nearestAirfieldKm ? nearestAirfieldKm(f.lat, f.lon) : -1;
        if (km >= 0 && km < airfieldKm) return false;   // landed at (or next to) an airfield
        if (a) {
            a->id = f.id; a->t = f.t; a->lat = f.lat; a->lon = f.lon; a->airfieldKm = km; a->flownSec = s.flyLast - s.flyStart;
            char b[200];
            if (km >= 0) snprintf(b, sizeof b, "possible out-landing at %.4f %.4f, %.1f km from the nearest airfield, after %.0f min of flight", f.lat, f.lon, km, a->flownSec / 60);
            else snprintf(b, sizeof b, "possible out-landing at %.4f %.4f, no airfield known nearby, after %.0f min of flight", f.lat, f.lon, a->flownSec / 60);
            a->text = b;
        }
        return true;
    }
    void clear() { st_.clear(); }
    static double distM(double la1, double lo1, double la2, double lo2) {
        const double r = M_PI / 180, x = (lo2 - lo1) * r * std::cos((la1 + la2) * 0.5 * r), y = (la2 - la1) * r;
        return 6371000.0 * std::hypot(x, y);
    }

private:
    struct St { double lastT = -1, flyStart = -1, flyLast = -1e9, stillStart = -1, stillLat = 0, stillLon = 0; bool reported = false; };
    std::unordered_map<uint32_t, St> st_;
};

} // namespace dect2
