// ADS-B arrivals and departures of one airport (FR24 airport page, spotter groups): an aircraft that came down near the airport and stopped
// (reported on the ground, or lost low) has landed; one that appears on the ground or low and climbing has taken off. With the time and the
// runway direction from its track. docs/ADSB-AIRPORT-3.9.md.
#pragma once
#include "dect2/adsb/adsb_tel.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct AirportBoardConfig {
    double lat = 0, lon = 0;
    int elevFt = 0;
    double radiusKm = 8;          // around the airport reference point
    int lowFt = 2000;             // above the field: "low"
};

struct AirportMovement {
    bool landing = true;          // false: take-off
    int64_t utc = 0;
    uint32_t icao = 0;
    std::string callsign, type, registration;
    double trackDeg = -1;         // the track on the runway (-1 unknown)
    std::string runway;           // "27" from the track (magnetic variation not applied)
};

class AirportBoard {
public:
    // one look at the aircraft (about once a second); returns the movements found now
    template <class Lookup> std::vector<AirportMovement> update(const std::vector<AdsbAircraft>& v, const AirportBoardConfig& c, int64_t utc, Lookup info);
    std::vector<AirportMovement> update(const std::vector<AdsbAircraft>& v, const AirportBoardConfig& c, int64_t utc);
    const std::vector<AirportMovement>& all() const { return done_; }   // oldest first
    static std::string runwayOf(double trackDeg);   // "09", "27", "36"
private:
    struct T { int64_t lastUtc = 0; bool low = false, descending = false, wasGround = false, landed = false, departed = false, near = false; double track = -1; int alt = 0; bool hasAlt = false; std::string cs; };
    std::vector<AirportMovement> step(const std::vector<AdsbAircraft>& v, const AirportBoardConfig& c, int64_t utc, std::map<uint32_t, std::pair<std::string, std::string>>& info);
    std::map<uint32_t, T> t_;
    std::vector<AirportMovement> done_;
};

template <class Lookup> std::vector<AirportMovement> AirportBoard::update(const std::vector<AdsbAircraft>& v, const AirportBoardConfig& c, int64_t utc, Lookup lookup) {
    std::map<uint32_t, std::pair<std::string, std::string>> info;
    std::vector<AirportMovement> m = step(v, c, utc, info);
    for (auto& x : m) { const auto ti = lookup(x.icao); x.type = ti.first; x.registration = ti.second; }
    for (auto& x : done_) if (x.type.empty() && x.utc == utc) { const auto ti = lookup(x.icao); x.type = ti.first; x.registration = ti.second; }
    return m;
}

}  // namespace dect2
