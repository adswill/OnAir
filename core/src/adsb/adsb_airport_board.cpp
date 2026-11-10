// ADS-B arrivals and departures: see adsb_airport_board.h.
#include "dect2/adsb/adsb_airport_board.h"
#include <cmath>
#include <cstdio>

namespace dect2 {

namespace {
double km(double la1, double lo1, double la2, double lo2) {
    const double r = 3.14159265358979 / 180, a = std::sin((la2 - la1) * r / 2), b = std::sin((lo2 - lo1) * r / 2);
    const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b;
    return 6371.0 * 2 * std::atan2(std::sqrt(h), std::sqrt(1 - h));
}
}  // namespace

std::string AirportBoard::runwayOf(double trk) {
    if (trk < 0) return "";
    int n = (int)std::lround(std::fmod(trk + 360, 360) / 10.0);
    if (n == 0) n = 36;
    char b[8]; std::snprintf(b, sizeof b, "%02d", n);
    return b;
}

std::vector<AirportMovement> AirportBoard::update(const std::vector<AdsbAircraft>& v, const AirportBoardConfig& c, int64_t utc) {
    std::map<uint32_t, std::pair<std::string, std::string>> info;
    return step(v, c, utc, info);
}

std::vector<AirportMovement> AirportBoard::step(const std::vector<AdsbAircraft>& v, const AirportBoardConfig& c, int64_t utc, std::map<uint32_t, std::pair<std::string, std::string>>&) {
    std::vector<AirportMovement> out;
    auto add = [&](bool landing, uint32_t icao, const T& t) {
        AirportMovement m;
        m.landing = landing; m.utc = utc; m.icao = icao; m.callsign = t.cs; m.trackDeg = t.track; m.runway = runwayOf(t.track);
        out.push_back(m);
        done_.push_back(m);
        if (done_.size() > 5000) done_.erase(done_.begin());
    };
    for (const AdsbAircraft& ac : v) {
        if (!ac.hasPos || ac.ageSec > 20) continue;
        const bool near = km(c.lat, c.lon, ac.lat, ac.lon) <= c.radiusKm;
        auto it = t_.find(ac.icao);
        const bool isNew = it == t_.end();
        T& t = t_[ac.icao];
        if (!ac.callsign.empty()) t.cs = ac.callsign;
        if (ac.hasHeading && ac.headingIsTrack) t.track = ac.headingDeg;
        const bool low = ac.ground || (ac.hasAlt && ac.altFt - c.elevFt < c.lowFt);
        const bool desc = ac.hasVrate ? ac.vrateFpm < -200 : (t.hasAlt && ac.hasAlt && ac.altFt < t.alt);
        const bool climb = ac.hasVrate ? ac.vrateFpm > 300 : (t.hasAlt && ac.hasAlt && ac.altFt > t.alt);
        if (near) {
            // landing: came down low near the field (seen descending) and now reports the ground
            if (ac.ground && t.descending && !t.landed) { add(true, ac.icao, t); t.landed = true; t.descending = false; }
            // take-off: was on the ground near the field (or first seen there low), now airborne, low and climbing
            if (!ac.ground && climb && low && (t.wasGround || isNew) && !t.departed) { add(false, ac.icao, t); t.departed = true; t.wasGround = false; }
            if (low && desc && !ac.ground && !t.departed) t.descending = true;
            if (ac.ground) t.wasGround = true;
        }
        t.low = low; t.near = near;
        if (ac.hasAlt) { t.alt = ac.altFt; t.hasAlt = true; }
        t.lastUtc = utc;
        if (!near && !low) { t.descending = false; t.landed = false; t.departed = false; t.wasGround = false; }   // away again: a new movement may follow later
    }
    // lost low near the field while coming down: landed (many transponders stop being heard below the radio horizon before they report the ground)
    for (auto it = t_.begin(); it != t_.end();) {
        T& t = it->second;
        if (utc - t.lastUtc >= 60) {
            if (!t.landed && t.near && t.low && t.descending) add(true, it->first, t);
            it = t_.erase(it);
        } else ++it;
    }
    return out;
}

}  // namespace dect2
