// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Thermals map from glider traffic (3.9, docs/SMART5-field-3.9.md item 2) for FLARM, OGN and ADS-L: a glider that circles (its track turns
// through 360 deg or more) inside a small circle while it climbs is in a thermal. The circle is taken after the wind drift is removed (a
// straight-line fit of the positions against time), so a thermal that drifts downwind still counts. Each thermal goes on the map with its
// centre, mean climb, base and top; thermals within mergeM of each other in the last mergeSec are one (several gliders in the same
// thermal raise its count). Only the aircraft's own broadcast is used; aircraft that ask not to be tracked never reach this (the receivers
// drop them). Pure logic, no clock.
#pragma once
#include "dect2/flarm/flarm_outland.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace dect2 {

struct Thermal {
    int id = 0;
    double firstT = 0, lastT = 0;        // seconds
    double lat = 0, lon = 0;             // centre (where it was last used)
    double climbMs = 0;                  // mean climb of the circling, weighted by time
    double baseM = 0, topM = 0;          // lowest and highest altitude circled (the aircraft's altitude reference)
    double seconds = 0;                  // circling time in it, all aircraft
    std::set<uint32_t> aircraft;
};

class ThermalMap {
public:
    double minClimbMs = 0.5;             // mean climb while circling
    double windowSec = 40;               // a full circle takes 20 - 35 s
    double maxRadiusM = 300;             // after the drift is removed
    double minSpeedMs = 12;              // flying
    double mergeM = 800, mergeSec = 2700;
    double keepSec = 3 * 3600;
    double strongMs = 2.0;               // report() flags thermals at least this strong

    // one position; `fresh` is set to the thermal when this fix created a new one (not when it joined one)
    bool add(const TrafficFix& f, Thermal* fresh = nullptr) {
        auto& a = ac_[f.id];
        if (!a.fixes.empty() && f.t <= a.fixes.back().t) return false;
        a.fixes.push_back(f);
        while (!a.fixes.empty() && f.t - a.fixes.front().t > windowSec) a.fixes.pop_front();
        if (f.reportsGround || f.speedMs < minSpeedMs) { a.current = -1; return false; }
        double cLat, cLon, climb;
        if (!circling(a.fixes, cLat, cLon, climb)) {
            if (a.current >= 0 && f.t - a.lastCircle > 20) a.current = -1;   // left the thermal
            return false;
        }
        const double dt = a.lastCircle > 0 && f.t - a.lastCircle < 10 ? f.t - a.lastCircle : 0;
        a.lastCircle = f.t;
        Thermal* th = a.current >= 0 ? find(a.current) : nullptr;
        bool created = false;
        if (!th) {
            th = nearest(cLat, cLon, f.t);
            if (!th) {
                Thermal n; n.id = ++next_; n.firstT = f.t; n.lat = cLat; n.lon = cLon; n.baseM = n.topM = f.altM; n.climbMs = climb;
                list_.push_back(n); th = &list_.back(); created = true;
            }
            a.current = th->id;
        }
        th->aircraft.insert(f.id);
        th->lastT = f.t;
        const double w = std::max(dt, 1.0);
        th->climbMs = (th->climbMs * th->seconds + climb * w) / (th->seconds + w);
        th->seconds += w;
        th->lat = cLat; th->lon = cLon;
        th->baseM = std::min(th->baseM, f.altM); th->topM = std::max(th->topM, f.altM);
        if (created && fresh) *fresh = *th;
        return created;
    }

    // forget thermals not used for keepSec and aircraft not heard for 10 minutes
    void prune(double now) {
        list_.erase(std::remove_if(list_.begin(), list_.end(), [&](const Thermal& t) { return now - t.lastT > keepSec; }), list_.end());
        for (auto it = ac_.begin(); it != ac_.end();) { if (it->second.fixes.empty() || now - it->second.fixes.back().t > 600) it = ac_.erase(it); else ++it; }
    }
    const std::deque<Thermal>& thermals() const { return list_; }
    // the thermals used in the last `ageSec`, strongest first
    std::vector<Thermal> active(double now, double ageSec = 1800) const {
        std::vector<Thermal> v;
        for (const auto& t : list_) if (now - t.lastT <= ageSec) v.push_back(t);
        std::sort(v.begin(), v.end(), [](const Thermal& a, const Thermal& b) { return a.climbMs > b.climbMs; });
        return v;
    }
    static std::string describe(const Thermal& t) {
        char b[160];
        snprintf(b, sizeof b, "thermal %.1f m/s, %.0f - %.0f m, %zu glider%s", t.climbMs, t.baseM, t.topM, t.aircraft.size(), t.aircraft.size() == 1 ? "" : "s");
        return b;
    }

private:
    struct Ac { std::deque<TrafficFix> fixes; int current = -1; double lastCircle = 0; };
    std::unordered_map<uint32_t, Ac> ac_;
    std::deque<Thermal> list_;
    int next_ = 0;

    Thermal* find(int id) { for (auto& t : list_) if (t.id == id) return &t; return nullptr; }
    Thermal* nearest(double lat, double lon, double t) {
        Thermal* best = nullptr; double bd = mergeM;
        for (auto& x : list_) {
            if (t - x.lastT > mergeSec) continue;
            const double d = metres(lat, lon, x.lat, x.lon);
            if (d <= bd) { bd = d; best = &x; }
        }
        return best;
    }
    static double metres(double la1, double lo1, double la2, double lo2) {
        const double k = 111320.0, x = (lo2 - lo1) * k * std::cos((la1 + la2) * 0.5 * 3.14159265358979 / 180), y = (la2 - la1) * k;
        return std::sqrt(x * x + y * y);
    }
    // circling: the window spans windowSec, the track turned through 360 deg one way, the drift-removed positions stay within maxRadiusM,
    // and the mean climb is minClimbMs or more. Gives the centre at the newest time and the climb.
    bool circling(const std::deque<TrafficFix>& v, double& cLat, double& cLon, double& climb) const {
        if (v.size() < 8 || v.back().t - v.front().t < windowSec * 0.9) return false;
        const double lat0 = v.front().lat, lon0 = v.front().lon, k = 111320.0, cl = std::cos(lat0 * 3.14159265358979 / 180);
        const size_t n = v.size();
        std::vector<double> x(n), y(n), t(n);
        for (size_t i = 0; i < n; i++) { x[i] = (v[i].lon - lon0) * k * cl; y[i] = (v[i].lat - lat0) * k; t[i] = v[i].t - v.front().t; }
        // turn: the sum of the changes of the track between consecutive positions
        double turn = 0, prev = 0; bool havePrev = false;
        for (size_t i = 1; i < n; i++) {
            const double dx = x[i] - x[i - 1], dy = y[i] - y[i - 1];
            if (dx * dx + dy * dy < 1) continue;
            const double c = std::atan2(dx, dy);
            if (havePrev) { double d = c - prev; while (d > 3.14159265358979) d -= 2 * 3.14159265358979; while (d < -3.14159265358979) d += 2 * 3.14159265358979; turn += d; }
            prev = c; havePrev = true;
        }
        if (std::fabs(turn) < 2 * 3.14159265358979) return false;
        // drift: x(t), y(t) straight lines
        double st = 0, sx = 0, sy = 0, stt = 0, stx = 0, sty = 0;
        for (size_t i = 0; i < n; i++) { st += t[i]; sx += x[i]; sy += y[i]; stt += t[i] * t[i]; stx += t[i] * x[i]; sty += t[i] * y[i]; }
        const double den = n * stt - st * st;
        if (den <= 0) return false;
        const double bx = (n * stx - st * sx) / den, ax = (sx - bx * st) / n, by = (n * sty - st * sy) / den, ay = (sy - by * st) / n;
        double maxR = 0;
        for (size_t i = 0; i < n; i++) { const double rx = x[i] - (ax + bx * t[i]), ry = y[i] - (ay + by * t[i]); maxR = std::max(maxR, std::sqrt(rx * rx + ry * ry)); }
        if (maxR > maxRadiusM) return false;
        // climb: the altitude change over the window, or the mean of the reported climb when the altitude does not move
        const double span = v.back().t - v.front().t;
        climb = (v.back().altM - v.front().altM) / span;
        if (climb == 0) { double s = 0; for (const auto& f : v) s += f.climbMs; climb = s / n; }
        if (climb < minClimbMs) return false;
        const double tx = ax + bx * t[n - 1], ty = ay + by * t[n - 1];
        cLat = lat0 + ty / k; cLon = lon0 + tx / (k * cl);
        return true;
    }
};

} // namespace dect2
