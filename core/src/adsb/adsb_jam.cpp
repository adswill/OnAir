// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/adsb/adsb_jam.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {

namespace {
int64_t cellKey(int la, int lo) { return (int64_t)(la + 200) * 1000 + (lo + 400); }   // la -180..179, lo -360..359
int cellLat(int64_t k) { return (int)(k / 1000) - 200; }
int cellLon(int64_t k) { return (int)(k % 1000) - 400; }
}

int GpsJamCell::level() const {
    if (aircraft < kGpsJamMinAircraft) return -1;
    const double r = (double)degraded / aircraft;
    return r < 0.1 ? 0 : r < 0.3 ? 1 : r < 0.6 ? 2 : 3;
}

bool GpsJamGrid::degradedAccuracy(int nacp, int nic) {
    return (nacp >= 0 && nacp < 7) || (nic >= 0 && nic < 6);
}

bool GpsJamGrid::impliesJump(double lat1, double lon1, double t1, double lat2, double lon2, double t2) {
    const double dt = t2 - t1;
    if (dt <= 0 || dt > 300) return false;
    const double p1 = lat1 * M_PI / 180, p2 = lat2 * M_PI / 180, dl = (lon2 - lon1) * M_PI / 180;
    const double c = std::sin(p1) * std::sin(p2) + std::cos(p1) * std::cos(p2) * std::cos(dl);
    const double nm = std::acos(std::max(-1.0, std::min(1.0, c))) * 3440.065;
    return nm / (dt / 3600.0) > kGpsJamJumpKt;
}

void GpsJamGrid::observe(const GpsJamObs& o) {
    if (o.ground || !(std::fabs(o.lat) < 90) || !(std::fabs(o.lon) <= 180)) return;
    bool jump = false;
    auto it = last_.find(o.icao);
    if (it != last_.end()) jump = impliesJump(it->second.lat, it->second.lon, it->second.t, o.lat, o.lon, o.timeSec);
    last_[o.icao] = {o.lat, o.lon, o.timeSec};
    const int la = (int)std::floor(o.lat / kGpsJamCellDeg), lo = std::min(359, (int)std::floor(o.lon / kGpsJamCellDeg));
    const bool bad = degradedAccuracy(o.nacp, o.nic) || jump;
    if (o.nacp < 0 && o.nic < 0 && !jump) return;   // nothing known about its accuracy (an old transponder): neither good nor bad, not counted
    Entry& e = cell_[cellKey(la, lo)][o.icao];
    // an aircraft counts once per cell and window; it is degraded when it was at any time in that window
    e.t = o.timeSec;
    e.bad = e.bad || bad;
    e.jump = e.jump || jump;
}

std::vector<GpsJamCell> GpsJamGrid::cells(double now, int minAircraft) {
    std::vector<GpsJamCell> out;
    for (auto c = cell_.begin(); c != cell_.end();) {
        GpsJamCell r;
        for (auto e = c->second.begin(); e != c->second.end();) {
            if (now - e->second.t > window_) { e = c->second.erase(e); continue; }
            r.aircraft++;
            if (e->second.bad) r.degraded++;
            if (e->second.jump) r.jumps++;
            ++e;
        }
        if (c->second.empty()) { c = cell_.erase(c); continue; }
        if (r.aircraft >= minAircraft) {
            const int64_t k = c->first;
            r.latIdx = cellLat(k); r.lonIdx = cellLon(k);
            out.push_back(r);
        }
        ++c;
    }
    for (auto l = last_.begin(); l != last_.end();) { if (now - l->second.t > window_) l = last_.erase(l); else ++l; }
    std::sort(out.begin(), out.end(), [](const GpsJamCell& a, const GpsJamCell& b) { return a.latIdx != b.latIdx ? a.latIdx < b.latIdx : a.lonIdx < b.lonIdx; });
    return out;
}

void GpsJamGrid::clear() { cell_.clear(); last_.clear(); }

std::string GpsJamGrid::toJson(const std::string& station, const std::vector<GpsJamCell>& cells) {
    if (cells.empty() || station.empty()) return std::string();
    std::string j = "{\"station\":\"" + station + "\",\"cell\":0.5,\"cells\":[";
    int n = 0;
    for (const auto& c : cells) {
        if (n >= kGpsJamMaxCells) break;
        char b[64];
        snprintf(b, sizeof b, "%s[%d,%d,%d,%d]", n ? "," : "", c.latIdx, c.lonIdx, std::min(c.aircraft, 300), std::min(c.degraded, std::min(c.aircraft, 300)));
        j += b; n++;
    }
    return j + "]}";
}

} // namespace dect2
