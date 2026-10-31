// AIS vessel filter, closest point of approach (CPA / TCPA) and geofence alerts.
//  - AisFilter: which stations a view or an alert looks at: station class (A, B, base, aid to navigation, SAR) and ship-type group.
//  - aisCpa(): distance and time of the closest approach of two moving stations on a flat local plane (good to well under 1 % inside 50 nm).
//  - AisAlertEngine: raises "cpa" when a vessel will pass the receiver (or a ship the user names the position of) closer than a limit within a
//    time, and "enter" / "exit" when a vessel crosses a geofence (circle or polygon). The receiver turns the alerts into the automation events
//    ais.cpa and ais.geofence (docs/AIS_ALERTS.md). Everything is off until the user sets a limit or a fence.
#pragma once
#include "ais_tel.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

// ---- filter
// Ship-type groups of ITU-R M.1371-5 table 50 (bit g of AisFilter::groups)
enum AisShipGroup { AIS_GRP_UNKNOWN = 0, AIS_GRP_FISHING, AIS_GRP_TOWING_OPS, AIS_GRP_SAILING_PLEASURE, AIS_GRP_HIGH_SPEED, AIS_GRP_SERVICE,
                    AIS_GRP_PASSENGER, AIS_GRP_CARGO, AIS_GRP_TANKER, AIS_GRP_OTHER, AIS_GRP_COUNT };
int aisShipGroup(int shipType);                  // -1 / not received -> AIS_GRP_UNKNOWN
const char* aisShipGroupText(int group);

struct AisFilter {
    uint32_t classes = 0x3F;                     // bit c = AisClass c shown
    uint32_t groups = (1u << AIS_GRP_COUNT) - 1; // bit g = AisShipGroup g shown
    bool movingOnly = false;                     // only stations with a known speed of at least minSogKn
    float minSogKn = 0.5f;
    bool matches(const AisVessel& v) const;
    bool active() const { return classes != 0x3F || groups != (1u << AIS_GRP_COUNT) - 1 || movingOnly; }
    std::string toText() const;                  // "classes=63;groups=1023;moving=0;sog=0.5" for the settings
    static AisFilter fromText(const std::string& s);   // unknown keys are ignored, a bad number keeps the default
};

// ---- CPA
struct AisCpa {
    bool valid = false;        // both have a position; the relative motion is known (else the current distance is returned with tcpa 0)
    double distNm = 0;         // current distance
    double dcpaNm = 0;         // distance at the closest approach (the current distance when the stations move apart)
    double tcpaMin = 0;        // minutes to the closest approach, negative: already passed
    double bearingDeg = 0;     // from a to b
};
// a: the first station (the receiver: sog 0 when it does not move), b: the other. Speeds in knots, courses in degrees true; a negative speed or course = unknown (treated as not moving).
AisCpa aisCpa(double latA, double lonA, double sogA, double cogA, double latB, double lonB, double sogB, double cogB);
double aisDistanceNm(double lat1, double lon1, double lat2, double lon2);

// ---- geofences
struct AisGeofence {
    std::string name;
    bool polygon = false;
    double lat = 0, lon = 0, radiusNm = 0;                   // circle
    std::vector<std::pair<double, double>> pts;             // polygon vertices (lat, lon), at least 3
    bool contains(double la, double lo) const;
};
// One fence per line:  name;lat,lon,radius_nm   or   name;poly;lat,lon lat,lon lat,lon ...   Empty lines and lines starting with # are skipped.
// Bad lines are listed in 'errors' (line number and reason) and skipped.
std::vector<AisGeofence> aisParseGeofences(const std::string& text, std::vector<std::string>* errors = nullptr);
std::string aisGeofencesToText(const std::vector<AisGeofence>& f);

// ---- alerts
struct AisAlertConfig {
    bool cpaOn = false;
    bool ownValid = false;                       // the position the CPA is measured from (the receiver position, or a moving ship the user enters)
    double ownLat = 0, ownLon = 0, ownSogKn = 0, ownCogDeg = 0;
    double cpaNm = 0.5;                          // alert when the closest approach is nearer than this ...
    double tcpaMin = 15;                         // ... and happens within this many minutes
    double maxAgeSec = 180;                      // positions older than this are not used
    double cooldownSec = 600;                    // the same vessel raises the CPA alert again after this
    AisFilter filter;                            // only these stations raise alerts
    std::vector<AisGeofence> fences;             // an empty list = no geofence alerts
};
struct AisAlert {
    double utc = 0;
    std::string kind;                            // "cpa", "enter", "exit"
    std::string fence;                           // geofence name (enter / exit)
    uint32_t mmsi = 0;
    std::string name;
    int shipType = -1, cls = AIS_CLASS_OTHER;
    double lat = 0, lon = 0, sogKn = -1, cogDeg = -1;
    double distNm = 0, dcpaNm = 0, tcpaMin = 0;  // cpa alerts (distance also for fences, from the own position when valid)
};

class AisAlertEngine {
public:
    void configure(const AisAlertConfig& c);     // the per-vessel memory stays (a fence edit does not re-raise "enter" for ships already inside)
    const AisAlertConfig& config() const { return cfg_; }
    // Call for every position update of a vessel. nowUtc: the time of that position report. Returns the alerts it raises (usually none).
    std::vector<AisAlert> update(const AisVessel& v, double nowUtc);
    void clear() { st_.clear(); }
private:
    struct State { uint32_t inside = 0; bool known = false; double lastCpa = -1e18; bool cpaActive = false; };
    AisAlertConfig cfg_;
    std::map<uint32_t, State> st_;
};

} // namespace dect2
