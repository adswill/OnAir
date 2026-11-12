// What the radiosonde map draws besides the sondes (4.0): every launch site in view with its name (the known list of sonde_sites_known.h,
// the sites learned from the sondes heard, sonde_sites.h), the user's own station with its callsign (or the chase car, from a live GPS),
// and the online flight prediction (Tawhiri, the SondeHub predictor) of the selected sonde. Pure data, so the tests can check it.
#pragma once
#include "dect2/sonde/sonde_sites.h"
#include "dect2/sonde/sonde_sites_known.h"
#include "dect2/sonde/sonde_tel.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {

struct SondeMapBox { double south = -90, west = -180, north = 90, east = 180; };   // west > east: the view crosses the 180 degree line
struct SondeMapStation { bool set = false, live = false; double lat = 0, lon = 0; std::string label; };   // live: a GPS fix now (chase car)

struct SondeMapMark {
    enum Kind { KnownSite, LearnedSite, Station, ChaseCar };
    Kind kind = KnownSite;
    double lat = 0, lon = 0;
    std::string label;
    int launches = 0;          // learned sites: launches heard
};

// The marks inside the box. A learned site hides a known site within 15 km (it carries that site's name already); known sites are
// thinned to maxKnown when the view is zoomed far out. The station is labelled with its callsign ("My station" without one).
std::vector<SondeMapMark> sondeMapMarks(const SondeMapBox& box, const KnownSondeSites* known, const std::vector<SondeSite>& learned,
                                        const SondeMapStation& station, size_t maxKnown = 400);

// ---- Tawhiri, the SondeHub predictor (https://api.v2.sondehub.org/tawhiri; API: tawhiri.readthedocs.io, standard_profile) ----
namespace tawhiri {
struct Point { double lat = 0, lon = 0, altM = 0, unixT = 0; };
struct Request { double lat = 0, lon = 0, altM = 0, unixT = 0, ascentMs = 5, burstM = 30000, descentMs = 5; bool descending = false; };
// Tawhiri's descent rate is the rate at sea level; the parachute falls faster in thin air. OnAir's landing model (sonde_land.h:
// speed scales with exp(h / 14 km)) gives the sea-level rate from the measured one.
double seaLevelDescent(double vDownMs, double altM);
// The request for a sonde in flight, as ChaseMapper builds it: climbing, the measured climb (at least 0.1 m/s) up to the assumed burst
// (100 m above the sonde when it is already higher); falling, the burst 1 m above it (Tawhiri needs the burst above the start) and the
// sea-level rate from the measured fall. false (why) without a position, time or speed, or on the ground.
bool requestFor(const SondeInfo& s, double burstM, double descentMs, double wallUtc, Request& r, std::string* why = nullptr);
std::string url(const std::string& base, const Request& r);   // the longitude as 0 - 360, the time as RFC 3339
struct Result {
    bool ok = false;
    std::string error;
    std::vector<Point> path;     // the ascent and the descent, in time order
    bool hasBurst = false;
    Point burst, landing;
};
Result parse(const std::string& json);   // longitudes back to -180 .. 180
}  // namespace tawhiri

// One prediction at a time in the background, at most one request per sonde every minGapS (60 s): only when the user turned it on.
class SondePredictor {
public:
    ~SondePredictor();
    std::string base = "https://api.v2.sondehub.org";
    double minGapS = 60;
    // starts a request unless one is running or this sonde's last one is younger than minGapS; true when started
    bool request(const std::string& serial, const tawhiri::Request& r, double nowSteady);
    bool result(const std::string& serial, tawhiri::Result& out, double& madeSteady) const;   // the last answer for the sonde
    bool busy() const { return busy_.load(); }
    std::string lastUrl() const;
private:
    struct Done { tawhiri::Result r; double t = 0; };
    mutable std::mutex mu_;
    std::map<std::string, Done> done_;
    std::map<std::string, double> asked_;
    std::string lastUrl_;
    std::atomic<bool> busy_{false};
    std::thread th_;
};

}  // namespace dect2
