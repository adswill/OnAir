// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// One incident model for sea distress (3.7, docs/SMART-maritime-3.7.md). A DSC distress alert opens an incident for the MMSI that is in distress and joins:
//  - the last AIS position of that MMSI (and how far it is from the position the DSC call gave: a big gap is flagged),
//  - the AIS-SART / MOB / EPIRB-AIS devices (MMSI 970 / 972 / 974) heard near that position (or alone, when no DSC call was heard),
//  - the vessels within 20 nm, with the closest-approach numbers to the scene (they are the ships that can help),
//  - NAVTEX warnings that name the MMSI or the ship, hold a position near the scene, or name the sea (marine_navtex_geo.h).
// The distance and bearing from the receiver come with it, a pinned alert is raised once, and every join goes into one timeline.
// Pure logic: no radio, no clock of its own (every call is given the time), thread safe. The marine receiver feeds DSC calls and NAVTEX messages,
// the AIS receiver feeds every vessel position (see IncidentBoard::global()); nothing is sent anywhere.
#pragma once
#include "dect2/ais/ais_tel.h"
#include "marine_tel.h"
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {
namespace marine {

struct IncidentEvent {
    double utc = 0;
    std::string source;              // "DSC", "AIS", "AIS-SART", "MOB", "EPIRB", "NAVTEX", "Board"
    std::string text;
    int count = 1;                   // the same call heard again (repeats are merged)
};

struct IncidentNear {
    uint32_t mmsi = 0;
    std::string name;
    double distNm = 0, bearingDeg = 0;   // from the scene
    double dcpaNm = 0, tcpaMin = 0;      // closest approach to the scene on the present course (tcpa < 0: moving away)
    double sogKn = -1;
    double ageSec = 0;
};

struct IncidentNavtex {
    std::string header, subject, reason, excerpt;
    double utc = 0;
};

struct Incident {
    uint32_t id = 0;
    std::string kind;                // "DSC distress", "AIS-SART", "Man overboard", "EPIRB-AIS"
    std::string mmsi;                // the station in distress (a device's own number for a device-only incident)
    std::string name, country;
    double openedUtc = 0, lastUtc = 0;
    // position: the DSC one when the call had it, else the AIS one
    bool hasPos = false;
    double lat = 0, lon = 0;
    std::string posSource;           // "DSC", "AIS", "AIS-SART" ...
    bool hasDscPos = false, hasAisPos = false;
    double dscLat = 0, dscLon = 0, aisLat = 0, aisLon = 0, aisAgeSec = 0;
    double posGapNm = -1;            // DSC position against the last AIS position of the same MMSI (-1: one of them missing)
    bool posGapFlag = false;         // over the configured limit
    // from the receiver
    bool hasOwn = false;
    double distNm = 0, bearingDeg = 0;
    std::string nature;              // "Fire, explosion", "Sinking" ...
    int dscCalls = 0, relays = 0;
    bool acked = false;
    std::string ackedBy;
    std::vector<std::string> devices;   // MMSI of the AIS-SART / MOB / EPIRB-AIS joined to it
    bool test = false;               // a test transmission of a device (never pinned, never alerts)
    bool pinned = false;             // the alert for this incident has been raised
    bool closed = false;
    uint32_t mergedInto = 0;         // a device-only incident that a DSC incident took in (not listed)
    std::vector<IncidentEvent> timeline;
    std::vector<IncidentNear> nearby;
    std::vector<IncidentNavtex> navtex;
};

struct IncidentAlert {
    uint32_t id = 0;
    double utc = 0;
    std::string what;                // "opened", "position", "device", "ack", "navtex"
    std::string text;                // one line
    std::string kind, mmsi;          // of the incident
    bool hasPos = false;
    double lat = 0, lon = 0, distNm = -1;   // the scene, and its distance from the receiver (-1: not known)
};

struct IncidentConfig {
    bool enabled = true;
    double nearbyNm = 20;            // vessels this close to the scene are listed
    double nearbyMaxAgeSec = 900;    // their last position must be this fresh
    double joinNm = 5;               // a device this close to a DSC position belongs to the same incident
    double gapFlagNm = 5;            // DSC position against AIS position: flag over this
    double aisMaxAgeSec = 6 * 3600;  // the last AIS position of the MMSI is used when younger than this
    double navtexNm = 100;           // a NAVTEX position this close to the scene counts
    double navtexBeforeSec = 2 * 3600, navtexAfterSec = 24 * 3600;
    double repeatSec = 300;          // the same DSC text inside this is a repeat
    double closeAfterSec = 6 * 3600; // an incident with no news for this long is closed
    double testKeepSec = 600;
    size_t maxIncidents = 40;
    size_t maxNearby = 12;
};

class IncidentBoard {
public:
    IncidentBoard() = default;
    explicit IncidentBoard(const IncidentConfig& c) : cfg_(c) {}
    // The one the receivers feed (the marine and the AIS receiver run in different threads): a process-wide board.
    static IncidentBoard& global();

    void configure(const IncidentConfig& c);
    IncidentConfig config() const;
    void setOwn(bool valid, double lat, double lon);      // the receiver position (Settings > Receiver position)

    // ---- feeds
    void onDsc(const DscCall& c, double nowUtc);          // calls that failed their check are ignored
    void onAis(const AisVessel& v, double nowUtc);        // every position report (cheap: one map entry per MMSI)
    void onNavtex(const NavtexMessage& m, double nowUtc); // every complete message
    void tick(double nowUtc);                             // closes quiet incidents, drops old test records

    // ---- reading
    std::vector<Incident> snapshot() const;               // newest first; closed ones last
    std::vector<IncidentAlert> takeAlerts();              // the alerts raised since the last call (the receivers turn them into events)
    size_t openCount() const;
    void clear();

private:
    struct AisPoint { double lat = 0, lon = 0, utc = 0; float sog = -1, cog = -1; int navStatus = -1; int cls = AIS_CLASS_OTHER; std::string name, dest; };
    struct NavRec { NavtexMessage m; double utc = 0; std::vector<double> lat, lon; std::vector<std::string> posText; std::vector<int> areas; std::string upper; };

    Incident* findOpen(const std::string& mmsi);
    Incident& open(const std::string& kind, const std::string& mmsi, double now);
    void addEvent(Incident& i, double utc, const std::string& src, const std::string& text, bool mergeRepeat = true);
    void alert(Incident& i, double now, const std::string& what, const std::string& text);
    void refresh(Incident& i, double now, bool full);
    void joinDevices(Incident& i, double now);
    void joinNavtex(Incident& i, const NavRec& n, double now);
    void applyAis(Incident& i, const AisPoint& p, double now);
    void deviceSeen(uint32_t mmsi, const AisVessel& v, double now);
    void pickPosition(Incident& i);

    mutable std::mutex mu_;
    IncidentConfig cfg_;
    bool ownValid_ = false;
    double ownLat_ = 0, ownLon_ = 0;
    uint32_t nextId_ = 1;
    std::vector<Incident> inc_;
    std::vector<IncidentAlert> alerts_;
    std::map<uint32_t, AisPoint> ais_;
    std::vector<NavRec> nav_;
    std::map<uint32_t, double> lastNear_;                // per incident id: when the nearby list was last recomputed
};

// The receivers call this after they hand something to the global board: it raises the pinned alerts as the automation event "dsc.incident"
// (what, text, kind, mmsi, lat, lon, distance_nm, incident) and sends each line to `log`, and closes quiet incidents (at most every 10 s).
void incidentFlush(const std::function<void(const std::string&)>& log);

// ---- helpers the board and its tests share
double incidentDistanceNm(double lat1, double lon1, double lat2, double lon2);
double incidentBearingDeg(double lat1, double lon1, double lat2, double lon2);

} // namespace marine
} // namespace dect2
