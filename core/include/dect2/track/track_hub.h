// The tracks of all aircraft of one mode (or of all modes together), keyed by ICAO address or registration, plus the adapters that turn
// the receivers' aircraft records into fixes. The hub owns the shared wind field and the ground-station positions.
#pragma once
#include "track_imm.h"
#include "track_route.h"
#include <map>
#include <unordered_map>

namespace dect2 {
struct AdsbAircraft;
struct AcarsAircraft;
struct Vdl2Aircraft;
struct HfdlAircraft;
}

namespace dect2::track {

using Key = uint64_t;
Key keyFromIcao(uint32_t icao);
Key keyFromText(const std::string& registration);   // registrations of aircraft whose address is not known

// Fixes out of the receivers' records (position, altitude, speed, ... with the source specific noise). `now` is the clock of the record
// (signal seconds); returns false when the record carries nothing new (no position and no kinematics).
bool fixFromAdsb(const AdsbAircraft& a, double now, Fix& f);
bool fixFromAcars(const AcarsAircraft& a, Fix& f);
bool fixFromVdl2(const Vdl2Aircraft& a, Fix& f);
bool fixFromHfdl(const HfdlAircraft& a, Fix& f);

class TrackHub {
public:
    explicit TrackHub(const TrackOptions& o = TrackOptions()) : opt_(o) {}
    UpdateResult feed(Key k, const Fix& f);
    // text of a message: route, ETA, destination; sets the route hint of the track. `now` on the clock of the fixes; `utcSecOfDay` the time of day
    // that goes with `now` (to turn an ETA of the text into the fixes' clock); -1 when unknown.
    bool feedText(Key k, const std::string& label, const std::string& text, double now, double utcSecOfDay);
    void setRoute(Key k, const RouteHint& r);
    // ground stations: where the station is, and which aircraft talked to it last
    void setStation(uint32_t id, double lat, double lon, double rangeNm = 0);   // rangeNm 0: line of sight at the aircraft's altitude (VHF); HF stations give their usual reach
    void noteStation(Key k, uint32_t stationId, double t);
    // The estimate. For an aircraft that has no position at all but talked to a ground station with a known position, a coarse estimate
    // (the station's line-of-sight disc: `coarse` set, ellipse = disc) is returned instead.
    bool estimate(Key k, double now, PosEstimate& out, bool quick = false) const;
    void purge(double now, double maxAgeSec);
    size_t size() const { return e_.size(); }
    const TrackEstimator* find(Key k) const { auto i = e_.find(k); return i == e_.end() ? nullptr : &i->second.est; }
    WindField& winds() { return wind_; }
    const TrackOptions& options() const { return opt_; }
    bool useWindField = true;
private:
    struct Entry {
        TrackEstimator est;
        uint32_t station = 0; double stationT = -1e18;
        double lastAltM = 0; bool hasAlt = false; double lastLat = 0, lastLon = 0; bool hasPos = false;
        double lastT = -1e18;
        Entry(const TrackOptions& o) : est(o) {}
    };
    TrackOptions opt_;
    std::unordered_map<Key, Entry> e_;
    struct St { double lat, lon, rangeNm; };
    std::map<uint32_t, St> st_;
    WindField wind_;
};

// The map screens' hubs, so that other windows (Search everything) can ask for an aircraft's latest track position. UI thread only.
void registerHub(const TrackHub* h);
void unregisterHub(const TrackHub* h);
// The most recent fix of this aircraft in any registered hub, estimated at the time of that fix (no prediction); false when no hub has it.
bool liveFix(Key k, double& lat, double& lon);

}  // namespace dect2::track
