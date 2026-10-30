// VDL Mode 2 receiver telemetry, shared by the receiver and the UI.
// The first members of Vdl2Telemetry are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
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
    float levelDb = -120;
    bool hasPos = false;         // from its GSIF / XID ground station location parameter
    double lat = 0, lon = 0;
    std::string airports;        // from the XID airport coverage parameter
};

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
    std::vector<Vdl2Aircraft> aircraft;         // most recently heard first, at most 200
    std::vector<Vdl2GroundStation> stations;    // most recently heard first, at most 64
    std::vector<Vdl2ChannelInfo> channels;      // the channel list, by frequency
};

constexpr size_t kVdl2MsgMax = 400;

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
