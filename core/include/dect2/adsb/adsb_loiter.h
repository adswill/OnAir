// ADS-B orbit / holding detector (4.0): spots aircraft that fly circles or racetracks (holding patterns, survey and patrol flights, helicopters over an
// incident, military tankers) from their position fixes alone. The 3.9 manoeuvre alerts only react to a steep turn right now; this one looks at the
// whole recent path: the signed heading change added up, how far the aircraft really got from where it was, and the centre and radius of the loop.
// Pure code. Fixes should come about every 2 s to 30 s; an aircraft is forgotten after maxAgeS without a fix.
#pragma once
#include "dect2/track/track_geo.h"
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace dect2 {

struct AdsbLoiter {
    uint32_t icao = 0;
    double turns = 0;            // full turns in the window, always positive (direction in clockwise)
    bool clockwise = true;
    double centreLat = 0, centreLon = 0, radiusKm = 0;
    double sinceS = 0;           // when the window that shows the orbit starts
};

class AdsbLoiterDetector {
public:
    double windowS = 900;        // path looked at
    double minTurns = 0.9;       // turns needed (a holding pattern is one turn per 4 minutes; a racetrack counts as one turn per lap)
    double minSpeedKt = 40;      // slower is a hover or a taxiing aircraft, not an orbit
    double maxRadiusKm = 25;     // wider loops are a route that bends, not an orbit
    double maxAgeS = 300;
    // returns true when the aircraft is (still) in an orbit after this fix
    bool add(uint32_t icao, double tS, double lat, double lon) {
        auto& q = fx_[icao];
        if (!q.empty() && tS <= q.back().t) return state_.count(icao) != 0;
        q.push_back({tS, lat, lon});
        while (!q.empty() && q.front().t < tS - windowS) q.pop_front();
        evaluate(icao, tS);
        return state_.count(icao) != 0;
    }
    const AdsbLoiter* find(uint32_t icao) const { auto it = state_.find(icao); return it == state_.end() ? nullptr : &it->second; }
    std::vector<AdsbLoiter> all() const { std::vector<AdsbLoiter> v; for (auto& kv : state_) v.push_back(kv.second); return v; }
    void expire(double nowS) {
        for (auto it = fx_.begin(); it != fx_.end();) {
            if (it->second.empty() || nowS - it->second.back().t > maxAgeS) { state_.erase(it->first); it = fx_.erase(it); } else ++it;
        }
    }
private:
    struct Fx { double t, lat, lon; };
    void evaluate(uint32_t icao, double nowS) {
        const auto& q = fx_[icao];
        state_.erase(icao);
        if (q.size() < 8) return;
        // headings between fixes that are far enough apart to mean something (at least 300 m)
        std::vector<double> hd; double path = 0; size_t last = 0;
        for (size_t i = 1; i < q.size(); i++) {
            const double d = track::gcDistM(q[last].lat, q[last].lon, q[i].lat, q[i].lon);
            if (d < 300) continue;
            path += d; hd.push_back(track::gcBearingDeg(q[last].lat, q[last].lon, q[i].lat, q[i].lon)); last = i;
        }
        if (hd.size() < 6) return;
        double cum = 0;
        for (size_t i = 1; i < hd.size(); i++) cum += track::wrap180(hd[i] - hd[i - 1]);
        const double dur = q.back().t - q.front().t;
        if (dur <= 0 || path / dur / 0.514444 < minSpeedKt) return;
        const double turns = std::fabs(cum) / 360.0;
        if (turns < minTurns) return;
        double cl = 0, co = 0; for (const auto& f : q) { cl += f.lat; co += f.lon; }
        cl /= q.size(); co /= q.size();
        double rad = 0; for (const auto& f : q) rad = std::max(rad, track::gcDistM(cl, co, f.lat, f.lon));
        if (rad / 1000.0 > maxRadiusKm) return;
        AdsbLoiter l; l.icao = icao; l.turns = turns; l.clockwise = cum > 0; l.centreLat = cl; l.centreLon = co; l.radiusKm = rad / 1000.0; l.sinceS = q.front().t;
        state_[icao] = l; (void)nowS;
    }
    std::map<uint32_t, std::deque<Fx>> fx_;
    std::map<uint32_t, AdsbLoiter> state_;
};

} // namespace dect2
