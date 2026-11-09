// AIS anomaly engine (3.7, docs/SMART-maritime-3.7.md): consistency checks on what the receiver hears, the kind of analysis a traffic service does
// on the server side, here from one receiver and nothing else:
//  - "jump":       a position that no ship could have reached (speed over the limit since the last report),
//  - "clash":      one MMSI reporting from two places at once (two tracks that keep alternating): a clone or a spoofer,
//  - "aton_moved": an aid to navigation that is not where it was first heard (and does not say it is off position),
//  - "mmsi_class": the station class does not fit the MMSI (a ship number sending as a coast station or an aid, a coast number sending as a ship),
//  - "speed_type": speed no ship of that type makes, three reports in a row,
//  - "name_change": the name of an MMSI changed to something else,
//  - "dark":       a ship that went silent inside the part of the sea this receiver covers (a per-bearing coverage model says when silence means something);
//                  "reappeared" when it comes back.
// Pure logic, no clock (every call gets the time of the report). Alerts go to the automation event ais.anomaly and the AIS Alerts view.
#pragma once
#include "ais_tel.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct AisAnomalyConfig {
    bool enabled = true;
    bool jump = true, clash = true, aton = true, mmsiClass = true, speedType = true, nameChange = true, dark = true;
    double maxSpeedKn = 60;          // faster than this between two reports is a jump (high-speed craft: hscMaxKn)
    double hscMaxKn = 90;
    double jumpMinNm = 1.0;          // and the distance must be over this
    double clashSepNm = 5;           // the two tracks of a clash are further apart than this ...
    double clashWindowSec = 900;     // ... and both were updated inside this ...
    int clashHits = 2;               // ... at least this often
    double atonMoveNm = 0.15;        // an aid moved further than this from where it was first heard
    double darkMinutes = 30;         // silent for this long (and at least 20 x its usual report interval)
    double coverageFrac = 0.6;       // inside this fraction of the range heard on that bearing
    int coverageMinSamples = 20;     // reports in the bearing sector before the model is believed
    double cooldownSec = 1800;       // the same alert for the same MMSI is raised again after this
    bool ownValid = false;           // the receiver position: the coverage model (and "dark") need it
    double ownLat = 0, ownLon = 0;
};

struct AisAnomaly {
    double utc = 0;
    std::string kind;                // see above
    int severity = 1;                // 0 information, 1 warning, 2 alarm
    uint32_t mmsi = 0;
    std::string name;
    double lat = 0, lon = 0;
    std::string text;                // one line
};

class AisAnomalyEngine {
public:
    void configure(const AisAnomalyConfig& c) { cfg_ = c; }
    const AisAnomalyConfig& config() const { return cfg_; }
    // Call for every position update of a vessel; now = the time of that report. Also scans for dark ships (about every 30 s of that time).
    std::vector<AisAnomaly> update(const AisVessel& v, double now);
    void clear();
    // for the screen
    struct Stats { size_t tracked = 0; int sectorsKnown = 0; double maxRangeNm = 0; uint64_t raised = 0; };
    Stats stats() const;
    // the range heard on a bearing (90th percentile of the last 128 samples in its 10 degree sector, one sample per ship per two minutes), 0 when not known
    double sectorRangeNm(double bearingDeg) const;

private:
    enum Kind { KJump, KClash, KAton, KAtonOff, KClass, KSpeed, KName, KDark, KBack, KKinds };
    struct Track { double lat = 0, lon = 0, utc = 0; float sog = -1, cog = -1; int hits = 0; bool valid = false; bool lr = false; };
    struct State {
        Track a, b;
        double firstUtc = 0, lastUtc = 0, cool[KKinds] = {};
        double gaps[8] = {}; int gapN = 0, gapPos = 0;
        uint32_t reports = 0;
        std::string name;
        int speedRun = 0;
        bool anchored = false; double anchorLat = 0, anchorLon = 0;
        bool darkRaised = false;
        double lastSog = -1; int lastNav = -1; int cls = AIS_CLASS_OTHER; int shipType = -1;
        double lastRange = 0, lastBearing = 0, lastSample = 0; bool hasRange = false;
        uint32_t clashSeen = 0;
    };
    bool fits(const Track& t, double lat, double lon, double now, double maxKn, double slackNm) const;
    void raise(std::vector<AisAnomaly>& out, State& s, Kind k, const AisVessel& v, double now, int sev, const std::string& text);
    void scanDark(double now, std::vector<AisAnomaly>& out);
    double medianGap(const State& s) const;
    void sector(double range, double bearing);

    AisAnomalyConfig cfg_;
    std::map<uint32_t, State> st_;
    static constexpr int kSectors = 36, kRing = 128;
    float ring_[kSectors][kRing] = {};
    int ringN_[kSectors] = {}, ringPos_[kSectors] = {};
    double lastScan_ = 0;
    uint64_t raised_ = 0;
    std::map<uint32_t, std::string> names_;   // last name per MMSI shown in an alert for a vessel that is no longer in the table
};

} // namespace dect2
