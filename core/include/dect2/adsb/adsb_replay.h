// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Per-aircraft track replay: every position fix of an aircraft is kept (bounded), and the track can be played back at any time between its
// first and last fix with interpolated position and altitude, so the map can show "where was it ten minutes ago" or run a replay slider.
// Gaps longer than maxGapS are not bridged (the aircraft left coverage), so the replay never draws a made-up leg. Pure logic.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>
namespace dect2 {

struct ReplayFix { double t = 0, lat = 0, lon = 0, altFt = 0; };
struct ReplayPoint { bool valid = false; double lat = 0, lon = 0, altFt = 0; bool interpolated = false; };

class TrackReplay {
public:
    size_t maxFixes = 3000;       // per aircraft
    double maxGapS = 120;         // do not interpolate across a hole longer than this
    // returns false when the fix was ignored (out of range, not newer than the last one, a jump no aircraft can make)
    bool add(uint32_t icao, double t, double lat, double lon, double altFt) {
        if (std::fabs(lat) > 90 || std::fabs(lon) > 180 || (lat == 0 && lon == 0)) return false;
        auto& v = tracks_[icao];
        if (!v.empty()) {
            const ReplayFix& l = v.back();
            if (t <= l.t) return false;
            const double km = distKm(l.lat, l.lon, lat, lon);
            if (km / ((t - l.t) / 3600.0) > 2500.0 && km > 5) return false;   // faster than any aircraft: a bad CPR decode
        }
        v.push_back({t, lat, lon, altFt});
        if (v.size() > maxFixes) v.erase(v.begin(), v.begin() + (long)(v.size() - maxFixes));
        return true;
    }
    ReplayPoint at(uint32_t icao, double t) const {
        ReplayPoint p;
        auto it = tracks_.find(icao);
        if (it == tracks_.end() || it->second.empty()) return p;
        const auto& v = it->second;
        if (t < v.front().t || t > v.back().t) return p;
        auto hi = std::lower_bound(v.begin(), v.end(), t, [](const ReplayFix& f, double x) { return f.t < x; });
        if (hi->t == t) { p = {true, hi->lat, hi->lon, hi->altFt, false}; return p; }
        const ReplayFix& b = *hi;
        const ReplayFix& a = *(hi - 1);
        if (b.t - a.t > maxGapS) return p;
        const double f = (t - a.t) / (b.t - a.t);
        double dl = b.lon - a.lon;
        if (dl > 180) dl -= 360; else if (dl < -180) dl += 360;   // across the antimeridian
        double lon = a.lon + dl * f;
        if (lon > 180) lon -= 360; else if (lon < -180) lon += 360;
        p = {true, a.lat + (b.lat - a.lat) * f, lon, a.altFt + (b.altFt - a.altFt) * f, true};
        return p;
    }
    // first and last fix time (false when the aircraft is unknown)
    bool span(uint32_t icao, double& t0, double& t1) const {
        auto it = tracks_.find(icao);
        if (it == tracks_.end() || it->second.empty()) return false;
        t0 = it->second.front().t; t1 = it->second.back().t;
        return true;
    }
    // every aircraft that has a position at time t (the whole sky at that moment)
    std::vector<std::pair<uint32_t, ReplayPoint>> sky(double t) const {
        std::vector<std::pair<uint32_t, ReplayPoint>> out;
        for (const auto& kv : tracks_) { ReplayPoint p = at(kv.first, t); if (p.valid) out.push_back({kv.first, p}); }
        return out;
    }
    size_t aircraft() const { return tracks_.size(); }
    size_t fixes(uint32_t icao) const { auto it = tracks_.find(icao); return it == tracks_.end() ? 0 : it->second.size(); }
    // forget aircraft whose last fix is older than before
    void prune(double before) {
        for (auto it = tracks_.begin(); it != tracks_.end();)
            if (it->second.empty() || it->second.back().t < before) it = tracks_.erase(it); else ++it;
    }
    static double distKm(double la1, double lo1, double la2, double lo2) {
        const double r = 0.017453292519943295, dla = (la2 - la1) * r, dlo = (lo2 - lo1) * r;
        const double a = std::sin(dla / 2) * std::sin(dla / 2) + std::cos(la1 * r) * std::cos(la2 * r) * std::sin(dlo / 2) * std::sin(dlo / 2);
        return 12742.0 * std::asin(std::min(1.0, std::sqrt(a)));
    }
private:
    std::map<uint32_t, std::vector<ReplayFix>> tracks_;
};

}  // namespace dect2
