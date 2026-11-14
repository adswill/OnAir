// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 2 receiver telemetry, shared by the receiver and the UI.
// The first members of Vdl2Telemetry are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

// One AVLC frame that passed its check sequence, decoded as far as OnAir reads it.
struct Vdl2Message {
    uint64_t serial = 0;         // grows by one per message, never restarts
    double timeSec = 0;          // signal seconds at the start of the burst
    int64_t wallTime = 0;        // wall clock (Unix seconds) when it was decoded
    double freqHz = 0;           // channel
    float levelDb = -120;        // burst power, dBFS
    float snrDb = 0;             // burst power over the channel's noise floor
    float cfoHz = 0;             // carrier offset of the burst from the channel (the radio's tuning error included)
    int rsFixed = 0;             // octets the Reed-Solomon code corrected in the burst
    // AVLC
    uint32_t src = 0, dst = 0;   // 24-bit addresses
    int srcType = 0, dstType = 0;   // 1 aircraft, 4 ground station (ADM), 5 ground station (DEL), 7 all stations, else reserved
    bool fromAir = false;        // source is an aircraft (downlink)
    bool fromGround = false;     // source is a ground station (uplink)
    bool onGround = false;       // A/G bit of the destination address: the aircraft is on the ground
    bool response = false;       // C/R bit of the source address
    char frameKind = 0;          // 'I', 'S' or 'U'
    std::string frameName;       // "I", "RR", "UI", "XID", "GSIF", "XID_CMD_LE" ...
    int ns = -1, nr = -1;        // I / S frame sequence numbers
    bool pf = false;             // poll / final bit
    // what the information field carries
    std::string proto;           // "ACARS", "X.25", "CLNP", "XID", "COTP", "" (none or not known)
    std::string summary;         // one line for the list
    std::string detail;          // the decoded layers, several lines, for the detail pane
    // ACARS over AVLC
    bool acars = false;
    std::string reg, flight, label, labelText, msgNum, acarsText;
    char blockId = 0;
    bool acarsFinal = true;      // ETX; false: ETB, more blocks follow
    char acarsSeq = 0;           // downlink block sequence letter
    bool downlinkBlock = false;  // the ACARS block id said downlink
    bool part = false;           // an ETB block held back for a joined message: kept for the session log, hidden from the lists
    int acarsBlocks = 1;         // blocks joined into this message
    bool acarsIncomplete = false;// joined, but a block was missed or the end never came
    bool acarsCrcOk = false;
    // a position the message carried (ADS-C, ACARS text report, XID aircraft / ground station location)
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool hasAlt = false;
    int altFt = 0;
    std::string posKind;
    // ADS-C earth reference (track / ground speed / vertical speed), without a position in the same message
    bool hasKin = false;
    double kinTrackDeg = 0;
    double groundKt = 0;
    int vsFpm = 0;
};

struct Vdl2TrackPoint { double lat = 0, lon = 0; int altFt = 0; double time = 0; };

// Dead reckoning from a lat/lon fix (flat-earth nm, same as the aero test routes).
inline void vdl2MoveFlat(double latDeg, double lonDeg, double trackDeg, double groundKt, double dtSec, double& latOut, double& lonOut) {
    const double nm = groundKt * dtSec / 3600.0;
    const double a = trackDeg * dect2::math::kPi / 180.0;
    latOut = latDeg + nm * std::cos(a) / 60.0;
    lonOut = lonDeg + nm * std::sin(a) / (60.0 * std::cos(latDeg * dect2::math::kPi / 180.0));
}

inline double vdl2DistNm(double lat1, double lon1, double lat2, double lon2) {
    using dect2::math::kPi;
    const double r = kPi / 180.0;
    const double la1 = lat1 * r, la2 = lat2 * r, dlo = (lon2 - lon1) * r, dla = (lat2 - lat1) * r;
    const double h = std::sin(dla * 0.5) * std::sin(dla * 0.5) + std::cos(la1) * std::cos(la2) * std::sin(dlo * 0.5) * std::sin(dlo * 0.5);
    return 2.0 * std::asin(std::min(1.0, std::sqrt(h))) * 3440.065;
}

constexpr double kVdl2PredictMaxSec = 45.0 * 60.0;   // do not extrapolate more than 45 min past the last fix

struct Vdl2Aircraft {
    uint32_t icao = 0;           // 24-bit address
    std::string reg, flight;     // from ACARS (registration, flight id) or an ADS-C flight id group
    uint32_t messages = 0;
    double freqHz = 0;           // channel of the last frame
    double lastHeardSec = 0;
    int64_t lastWall = 0;
    float levelDb = -120;
    uint32_t groundStation = 0;  // the ground station it last talked to
    bool onGround = false;
    bool hasPos = false;
    double lat = 0, lon = 0;
    bool hasAlt = false;
    int altFt = 0;
    std::string posKind;
    double posTime = 0;
    bool hasTrack = false;       // from ADS-C earth ref, the report, or the last two positions
    double trackDeg = 0;
    bool hasKin = false;         // ADS-C earth reference ground speed (and vsFpm)
    double groundKt = 0;
    int vsFpm = 0;
    double kinTime = 0;          // signal time of the last kinematics update
    std::vector<Vdl2TrackPoint> track;   // oldest first, at most 24
    struct StationHop { uint32_t addr = 0; double timeSec = 0; };
    std::vector<StationHop> hops;        // the ground stations it talked to, oldest first, at most 5 (its handoff chain)
};

// Last known fix at posTime, optionally advanced to nowSec using ADS-C earth ref or speed from the last track segment.
inline bool vdl2AircraftPositionNow(const Vdl2Aircraft& c, double nowSec, double& lat, double& lon, int& altFt, bool& predicted) {
    predicted = false;
    if (!c.hasPos) return false;
    lat = c.lat;
    lon = c.lon;
    altFt = c.hasAlt ? c.altFt : 0;
    const double dt = nowSec - c.posTime;
    if (dt < 2.0 || c.onGround || dt > kVdl2PredictMaxSec) return true;
    double kt = 0;
    const double trk = c.trackDeg;
    if (c.hasKin && c.groundKt >= 15.0) kt = c.groundKt;
    else if (c.hasTrack && c.track.size() >= 2) {
        const Vdl2TrackPoint& p0 = c.track[c.track.size() - 2];
        const Vdl2TrackPoint& p1 = c.track.back();
        const double seg = p1.time - p0.time;
        if (seg > 3.0 && seg < 7200.0) kt = vdl2DistNm(p0.lat, p0.lon, p1.lat, p1.lon) / (seg / 3600.0);
    }
    if (kt < 15.0 || !c.hasTrack) return true;
    vdl2MoveFlat(c.lat, c.lon, trk, kt, dt, lat, lon);
    if (c.hasAlt && c.hasKin && std::abs(c.vsFpm) > 50) altFt = c.altFt + (int)std::lround(c.vsFpm * dt / 60.0);
    predicted = true;
    return true;
}

// Distance in nautical miles from the receiver (refLat, refLon) to the aircraft's position now (as vdl2AircraftPositionNow); false without a position (#394)
inline bool vdl2AircraftRangeNm(const Vdl2Aircraft& c, double nowSec, double refLat, double refLon, double& nm) {
    double lat = 0, lon = 0;
    int alt = 0;
    bool pred = false;
    if (!vdl2AircraftPositionNow(c, nowSec, lat, lon, alt, pred)) return false;
    nm = vdl2DistNm(refLat, refLon, lat, lon);
    return true;
}

struct Vdl2GroundStation {
    uint32_t addr = 0;
    int type = 4;
    uint32_t messages = 0;
    double freqHz = 0;
    double lastHeardSec = 0;
    double lastSeenSec = 0;      // the last frame from or to it (the "remove after" time counts from here; not saved in the results file)
    float levelDb = -120;
    bool hasPos = false;         // from its GSIF / XID ground station location parameter
    double lat = 0, lon = 0;
    std::string airports;        // from the XID airport coverage parameter
    // learned from the aircraft that talk to it, when they have a position (no GSIF location heard, or for the coverage)
    bool hasEstimate = false;    // an estimate from the aircraft positions; a taxiing aircraft counts for 25 airborne ones
    double estLat = 0, estLon = 0;
    int estFixes = 0;            // positions behind the estimate
    double sumW = 0, sumX = 0, sumY = 0, sumZ = 0;
    float coverageNm = 0;        // farthest aircraft with a position that talked to it, from its location or estimate
    uint32_t aircraftHeard = 0;  // frames with a located aircraft
    // reported locations: a ground station does not move, so hasPos / lat / lon are the consensus of these (vdl2StationReport), not the last report
    std::vector<std::pair<double, double>> posHist;   // the last valid reports (not saved in the results file)
    bool hasPending = false;     // one report heard, no second one agreeing with it yet: drawn as unconfirmed, hasPos stays false
    double pendLat = 0, pendLon = 0;
    uint32_t posOutliers = 0;    // reports that disagreed with the consensus (a corrupted coordinate) and were ignored
    uint32_t posRejected = 0;    // reports with impossible coordinates
};

// One aircraft position heard talking to this station: the estimate on the sphere and the coverage radius
inline void vdl2StationLearn(Vdl2GroundStation& g, double lat, double lon, bool onGround) {
    using dect2::math::kPi;
    const double r = kPi / 180.0, w = onGround ? 25.0 : 1.0;
    g.sumW += w;
    g.sumX += w * std::cos(lat * r) * std::cos(lon * r);
    g.sumY += w * std::cos(lat * r) * std::sin(lon * r);
    g.sumZ += w * std::sin(lat * r);
    g.estFixes++;
    g.aircraftHeard++;
    if (g.estFixes >= 3) {
        const double h = std::hypot(g.sumX, g.sumY);
        g.estLat = std::atan2(g.sumZ, h) / r;
        g.estLon = std::atan2(g.sumY, g.sumX) / r;
        g.hasEstimate = true;
    }
    if (g.hasPos || g.hasEstimate) g.coverageNm = std::max(g.coverageNm, (float)vdl2DistNm(g.hasPos ? g.lat : g.estLat, g.hasPos ? g.lon : g.estLon, lat, lon));
}

// A coordinate a station can really report: on the globe and not the empty 0,0 a failed decode gives.
inline bool vdl2ValidCoord(double lat, double lon) {
    return std::isfinite(lat) && std::isfinite(lon) && std::fabs(lat) <= 90.0 && std::fabs(lon) <= 180.0 && !(lat == 0.0 && lon == 0.0);
}

enum class Vdl2PosResult { Rejected, Outlier, Unconfirmed, Confirmed };

// One reported location of a ground station. The reports are kept (the last 16); the position is the mean of the largest group of reports
// within 5 km of each other, and it needs two agreeing reports. A single report far from that group (one wrong bit in a coordinate) is an
// outlier: counted and ignored. A station loaded from a results file (hasPos, no history) counts its saved position as two reports.
inline Vdl2PosResult vdl2StationReport(Vdl2GroundStation& g, double lat, double lon) {
    constexpr double kAgreeNm = 2.7;   // 5 km
    constexpr size_t kKeep = 16;
    if (!vdl2ValidCoord(lat, lon)) { g.posRejected++; return Vdl2PosResult::Rejected; }
    if (g.hasPos && g.posHist.empty()) g.posHist.assign(2, {g.lat, g.lon});
    g.posHist.emplace_back(lat, lon);
    if (g.posHist.size() > kKeep) g.posHist.erase(g.posHist.begin());
    const auto& h = g.posHist;
    size_t bestN = 0, bestI = 0;
    for (size_t i = 0; i < h.size(); i++) {
        size_t n = 0;
        for (size_t j = 0; j < h.size(); j++) n += vdl2DistNm(h[i].first, h[i].second, h[j].first, h[j].second) <= kAgreeNm;
        if (n >= bestN) { bestN = n; bestI = i; }   // ties: the newer report
    }
    if (bestN < 2) {   // nothing agrees yet
        if (g.hasPos) { g.posOutliers++; return Vdl2PosResult::Outlier; }
        g.hasPending = true; g.pendLat = lat; g.pendLon = lon;
        return Vdl2PosResult::Unconfirmed;
    }
    double sx = 0, sy = 0, sz = 0;
    const double r = dect2::math::kPi / 180.0;
    for (const auto& q : h) if (vdl2DistNm(h[bestI].first, h[bestI].second, q.first, q.second) <= kAgreeNm) {
        sx += std::cos(q.first * r) * std::cos(q.second * r); sy += std::cos(q.first * r) * std::sin(q.second * r); sz += std::sin(q.first * r);
    }
    const double cl = std::atan2(sz, std::hypot(sx, sy)) / r, co = std::atan2(sy, sx) / r;
    g.hasPos = true; g.hasPending = false; g.lat = cl; g.lon = co;
    if (vdl2DistNm(cl, co, lat, lon) > kAgreeNm) { g.posOutliers++; return Vdl2PosResult::Outlier; }
    return Vdl2PosResult::Confirmed;
}

struct Vdl2ChannelInfo {
    double freqHz = 0;
    bool inBand = false;         // inside the sample band (otherwise not received)
    bool active = false;         // a burst in the last 2 s
    float levelDb = -120;        // power of the last burst, dBFS
    float noiseDb = -120;        // noise floor of the channel, dBFS
    float snrDb = 0;             // of the last burst
    uint64_t bursts = 0;         // training sequences found
    uint64_t good = 0;           // bursts with at least one good frame
    uint64_t bad = 0;            // bursts whose header or Reed-Solomon blocks failed, or no frame passed its check
    uint64_t frames = 0;         // AVLC frames with a good check sequence
    double lastHeardSec = 0;     // signal time of the last good burst (0 = none)
    float lastCfoHz = 0;
};

struct Vdl2Telemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 searching, 1 bursts (training sequences found), 2 decoding (a good frame in the last 30 s)
    double cfoHz = 0;            // the radio's tuning error as measured from the bursts (applied to every channel)
    float snrDb = 0;             // of the last good burst
    bool dataValid = false;      // a frame has been decoded
    uint64_t blocksOk = 0;       // AVLC frames with a good check
    uint64_t blocksBad = 0;      // bursts that failed
    // ---- the rest is for the VDL2 screens
    double centerHz = 0;         // what the radio is tuned to (the dial frequency, 0 for a file)
    double nowSec = 0;
    double inputRate = 0, channelRate = 0;
    uint64_t rsFixedTotal = 0;   // octets corrected by the Reed-Solomon code
    uint64_t headerFixed = 0;    // headers that needed their FEC
    uint64_t fcsBad = 0;         // frames that failed their check sequence
    uint64_t positionsTotal = 0;
    std::vector<Vdl2Message> messages;          // newest first, at most kVdl2MsgMax
    std::vector<Vdl2Aircraft> aircraft;         // most recently heard first, expired after 6 h, at most dect2::limits::kAircraft
    std::vector<Vdl2GroundStation> stations;    // most recently heard first, expired after 6 h, at most dect2::limits::kGroundStations
    std::vector<Vdl2ChannelInfo> channels;      // the channel list, by frequency
};

constexpr size_t kVdl2MsgMax = 2000;

inline std::string vdl2Summary(const Vdl2Telemetry& t) {
    int inBand = 0, act = 0;
    for (const auto& c : t.channels) { inBand += c.inBand; act += c.active; }
    char b[180];
    if (!t.channels.empty() && !inBand) return "VDL2: no channel inside the band, tune nearer 136.85 MHz";
    if (t.state == 0) { snprintf(b, sizeof b, "VDL2: listening to %d channel%s", inBand, inBand == 1 ? "" : "s"); return b; }
    snprintf(b, sizeof b, "VDL2: %s  %d of %d channels active  %llu frames  %llu bad  %zu aircraft  %zu ground stations", t.state == 2 ? "decoding" : "bursts",
             act, inBand, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.aircraft.size(), t.stations.size());
    return b;
}

} // namespace dect2
