// Server > Send data to map server: opted-in POST of decoded positions to map.on-air.dev /ingest.
// Only positions the signal itself sent (no TLE catalogue, receiver GNSS, RID operator, FLARM stealth, cell towers, synthetic test signal).
#include <cstdlib>
#include "dect2/common/thread_name.h"
#include "map_name.h"
#include "derived_kin.h"
#include "../app.h"
#include "../state/receivers.h"
#include "map_ask.h"
#include "../rooms/rooms.h"
#include "../adsb/adsb_acars.h"
#include "../transcribe/transcribe_app.h"
#ifndef ONAIR_VERSION
#define ONAIR_VERSION "dev"
#endif
#include "dect2/flarm/flarm_proto.h"
#include "dect2/gnss/gnss_tel.h"
#include "dect2/ham/ham_mapfeed.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <ctime>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>

bool vdl2EstimatedPosition(uint32_t icao, double now, double& lat, double& lon, double& ageSec);   // app/vdl2/vdl2_ui.cpp
bool hfdlEstimatedPosition(uint32_t key, double now, double& lat, double& lon, double& ageSec);   // app/hfdl/hfdl_ui.cpp
derivedkin::Info acarsKinematics(const dect2::AcarsAircraft& c, double now);   // app/acars/acars_ui.cpp
derivedkin::Info vdl2Kinematics(const dect2::Vdl2Aircraft& c, double now);     // app/vdl2/vdl2_ui.cpp
derivedkin::Info hfdlKinematics(const dect2::HfdlAircraft& c, double now);     // app/hfdl/hfdl_ui.cpp

namespace {

constexpr const char* kDefaultOrigin = "https://map.on-air.dev";
constexpr double kMinInterval = 4.0;
constexpr double kHeartbeatSec = 60.0;
constexpr double kRetrySec = 8.0;
constexpr int kFailPopupAfter = 3;
constexpr size_t kMaxTracks = 250;
// Max position age (seconds) before OnAir omits a fix from ingest (per kind / link).
double maxIngestPosAgeSec(const char* kind, const std::string& src) {
    if (!kind || !*kind) return 300;
    const std::string s = src;
    if (!std::strcmp(kind, "aircraft")) {
        if (s == "adsb" || s == "acars" || s == "hfdl") return 30;
        if (s == "sat") return 60;
        return 45;
    }
    if (!std::strcmp(kind, "vessel")) return 90;
    if (!std::strcmp(kind, "flarm")) return 30;
    if (!std::strcmp(kind, "drone")) return 45;
    if (!std::strcmp(kind, "aprs") || !std::strcmp(kind, "ham")) return 120;
    if (!std::strcmp(kind, "sonde")) return 900;
    if (!std::strcmp(kind, "sat")) return 180;
    if (!std::strcmp(kind, "gnss")) return 120;
    if (!std::strcmp(kind, "dsc")) return 600;
    if (!std::strcmp(kind, "buoy")) return 7200;
    if (!std::strcmp(kind, "station")) return 3600;
    return 300;
}
constexpr double kIngestMaxAdsbPosAgeSec = 30.0;   // same as adsb_map acStaleSec: do not publish a frozen CPR fix

std::atomic<bool> gBusy{false};
std::mutex gThMu;
std::thread gTh;
bool gAskConsent = false;
bool gAskConnecting = false;
bool gPingNow = false;
enum { kOff = 0, kConnecting = 1, kOk = 2, kRetry = 3 };
std::atomic<int> gPhase{kOff};
std::atomic<int> gLastN{0};
std::atomic<uint64_t> gTotalN{0};
std::atomic<bool> gNeedRetryPopup{false};
std::atomic<int> gFails{0};
std::atomic<int64_t> gLastOkMs{0};
std::atomic<int64_t> gLastTryMs{0};
std::mutex gMapLogMu;
std::deque<std::string> gMapLog;
constexpr size_t kMapLogMax = 100;
bool gToastRetry = false;

void mapLog(const char* fmt, ...) {
    char msg[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char ts[32];
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S UTC", &tm);
    std::string line = std::string(ts) + "  " + msg;
    std::lock_guard<std::mutex> lk(gMapLogMu);
    gMapLog.push_back(std::move(line));
    while (gMapLog.size() > kMapLogMax) gMapLog.pop_front();
}

const char* phaseName(int ph) {
    switch (ph) {
    case kConnecting: return "connecting";
    case kOk: return "connected";
    case kRetry: return "retrying";
    default: return "off";
    }
}
bool gTxHold = false;                // a transmission is in progress, or just ended: do not post, and do not let the user turn sending back on
bool gTxPausedSend = false;          // the connection was on when transmit started (restored only if the radio never keyed)
int64_t gToastTxUntil = 0;

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

App& rootApp(App& a) {
    if (App* r = receiverById(1)) return *r;
    return a;
}

bool validPos(double lat, double lon) {
    if (!std::isfinite(lat) || !std::isfinite(lon)) return false;
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
    if (lat == 0 && lon == 0) return false;
    return true;
}

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

std::string makeStationId() {
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device rd;
    std::string s(20, '0');
    for (char& c : s) c = abc[rd() % 62];
    return s;
}

bool stationOk(const std::string& s) {
    if (s.size() < 8 || s.size() > 64) return false;
    for (unsigned char c : s) if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

void ensureStation(App& a) {
    if (stationOk(a.mapStationId)) return;
    a.mapStationId = makeStationId();
    savePrefs(a);
}

std::string originOf(const App& a) {
    std::string o = a.mapServer;
    if (o.empty()) o = plat::basePrefs().getS("mapServer", "");
    if (o.empty()) o = kDefaultOrigin;
    while (!o.empty() && o.back() == '/') o.pop_back();
    return o;
}

struct Track {
    const char* kind = "";
    std::string id, label, sub, extra, src, cat;
    double lat = 0, lon = 0;
    bool hasHeard = false;
    double heardAt = 0;              // signal time of the last position (0 when not known)
    bool hasAlt = false, hasHdg = false, hasSpd = false, hasVrate = false, hasSquawk = false;
    double alt = 0, hdg = 0, spd = 0, vrate = 0, age = 0;
    int squawk = 0;
    double dfFreq = 0, dfSigma = 0, dfConf = 0;   // kind "doa": frequency (Hz), bearing sigma (deg) and confidence (0..1) sent next to hdg
    bool dv = false;                 // speed / heading / vertical rate are derived from position reports (or estimated), not broadcast: sent as "dv":1
    bool est = false;                // the position is an estimate (dead reckoning, core/src/track), not a received fix: sent as "est":1
    std::vector<std::pair<double, double>> path;
    std::vector<dect2::AcarsLogEntry> msgs;   // interesting ACARS messages not sent yet (only when the user chose to share them)
};

// Speed, heading and vertical rate of a track that only has positions: the measured ones stay; the others come from the estimator or from
// two position reports (core/include/dect2/geo/kinematics.h) and are flagged. The heading is sent only when it is good enough to turn an icon.
void applyKin(Track& t, const derivedkin::Info& k, bool measuredGs, bool measuredTrack) {
    if (!measuredTrack) t.hasHdg = false;
    if (!measuredGs) t.hasSpd = false;
    const dect2::geo::KinValue& v = k.v;
    bool flagged = false;
    if (!measuredGs && v.hasGs) { t.hasSpd = true; t.spd = v.gsKt; flagged = true; }
    if (!measuredTrack && v.hasTrack && v.trackOk) { t.hasHdg = true; t.hdg = v.trackDeg; flagged = true; }
    if (!t.hasVrate && v.hasVs && v.src != dect2::geo::KinSource::Measured) { t.hasVrate = true; t.vrate = v.vsFpm; flagged = true; }
    if (flagged) t.dv = true;
}

struct RunGate {
    uint64_t epoch = 0;
    bool primed = false;
    std::unordered_map<std::string, double> heard;   // last hear at the start of this run; missing = not present then
};
std::unordered_map<int, RunGate> gRun;
RunGate* gG = nullptr;

std::string aircraftMapId(uint32_t icao, const std::string& flight, const std::string& reg) {
    if (icao & 0xFFFFFFu) {
        char b[8];
        snprintf(b, sizeof b, "%06X", icao & 0xFFFFFFu);
        return b;
    }
    if (!reg.empty()) return reg;
    if (!flight.empty()) return flight;
    return {};
}

bool isIcaoHexId(const std::string& id) {
    if (id.size() != 6) return false;
    for (unsigned char c : id) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string normalizeMmsiId(const std::string& s) {
    std::string d;
    for (unsigned char c : s) if (c >= '0' && c <= '9') d += (char)c;
    if (d.size() >= 9) return d.substr(d.size() - 9);
    return d;
}

bool isMmsiId(const std::string& id) {
    if (id.size() != 9) return false;
    for (unsigned char c : id) if (c < '0' || c > '9') return false;
    return true;
}

bool fresherThan(const Track& a, const Track& b) {
    if (a.age > 0 && b.age > 0) return a.age < b.age;
    if (a.hasHeard && b.hasHeard) return a.heardAt > b.heardAt + 0.2;
    return true;
}

void addTrack(std::vector<Track>& v, std::unordered_set<std::string>& seen, Track t) {
    if (!t.kind || !*t.kind || t.id.empty() || !validPos(t.lat, t.lon)) return;
    if (t.age > maxIngestPosAgeSec(t.kind, t.src)) return;
    if (t.kind && std::strcmp(t.kind, "aircraft") == 0 && t.src == "adsb" && t.age > kIngestMaxAdsbPosAgeSec) return;
    if (t.id.size() > 80) t.id.resize(80);
    const std::string key = std::string(t.kind) + ":" + t.id;
    const double ht = t.hasHeard ? t.heardAt : -1;
    if (gG && !gG->primed) { gG->heard[key] = ht; return; }   // first look this run: remember leftovers, send nothing yet
    if (gG && gG->primed) {
        auto it = gG->heard.find(key);
        if (it != gG->heard.end()) {
            if (ht < 0 || ht <= it->second + 0.2) return;   // not heard again since the receiver started
            t.path.clear();                                 // saved trail is not this run
        }
    }
    if (!seen.insert(key).second) {
        for (Track& existing : v) {
            if (std::string(existing.kind) + ":" + existing.id != key) continue;
            if (fresherThan(t, existing)) existing = std::move(t);
            return;
        }
        return;
    }
    if (v.size() >= kMaxTracks) return;
    v.push_back(std::move(t));
}

void fromMarkers(std::vector<Track>& v, std::unordered_set<std::string>& seen, const std::vector<dect2::MapMarker>& ms, double now) {
    for (const dect2::MapMarker& m : ms) {
        Track t;
        t.id = m.id;
        t.label = m.label.empty() ? m.id : m.label;
        if (m.layer == dect2::LayerAdsb) { t.kind = "aircraft"; t.sub = "ADS-B"; t.src = "adsb"; if (m.hasAlt) { t.hasAlt = true; t.alt = m.altM / 0.3048; } }
        else if (m.layer == dect2::LayerAis) {
            t.kind = "vessel"; t.sub = "AIS"; t.src = "ais";
            const std::string mmsi = normalizeMmsiId(m.id);
            if (!mmsi.empty()) t.id = mmsi;
            if (m.hasAlt) { t.hasAlt = true; t.alt = m.altM; }
        }
        else if (m.layer == dect2::LayerAprs) { t.kind = "aprs"; t.sub = "APRS"; t.src = "aprs"; if (m.hasAlt) { t.hasAlt = true; t.alt = m.altM; } }
        else if (m.layer == dect2::LayerSonde) { t.kind = "sonde"; t.sub = "sonde"; t.src = "sonde"; if (m.hasAlt) { t.hasAlt = true; t.alt = m.altM; } }
        else continue;
        t.lat = m.lat; t.lon = m.lon;
        t.age = m.ageSec;
        t.hasHeard = true;
        t.heardAt = now - m.ageSec;
        if (m.hasSpeed) { t.hasSpd = true; t.spd = m.speedKt; }
        if (m.headingDeg >= 0) { t.hasHdg = true; t.hdg = m.headingDeg; }
        if (!m.detail.empty()) t.extra = m.detail.substr(0, 120);
        for (const auto& p : m.track) if (validPos(p.first, p.second)) t.path.push_back(p);
        addTrack(v, seen, std::move(t));
    }
}

void collectFrom(App& rx, std::vector<Track>& v, std::unordered_set<std::string>& seen) {
    if (rx.engine.syntheticSource()) return;   // the built-in test signal is not traffic
    if (rx.devIdx >= 0 && rx.devIdx < (int)rx.devices.size() && rx.devices[(size_t)rx.devIdx].kind == DeviceInfo::Synthetic) return;
    if (!rx.engine.running()) return;          // saved lists are not live traffic
    RunGate& g = gRun[rx.rxId];
    const uint64_t ep = rx.engine.startEpoch();
    if (g.epoch != ep) { g.heard.clear(); g.primed = false; g.epoch = ep; }
    gG = &g;
    // Same lists the UI shows (a.rx), not a second snapshot from the decoder.
    dect2::AdsbTelemetry adsb;
    if (rx.engine.adsb().telemetry(adsb, 0)) {
        fromMarkers(v, seen, dect2::markersFromAdsb(adsb), adsb.timeSec);
        for (Track& t : v) {
            if (t.kind && std::strcmp(t.kind, "aircraft") == 0 && t.src == "adsb") {
                for (const auto& a : adsb.aircraft) {
                    char id[16]; snprintf(id, sizeof id, "%06X", a.icao & 0xFFFFFF);
                    if (t.id != id) continue;
                    t.cat = a.category;
                    if (a.hasSquawk) { t.hasSquawk = true; t.squawk = a.squawk; }
                    if (a.hasVrate) { t.hasVrate = true; t.vrate = a.vrateFpm; }
                    t.age = a.posAgeSec > 0 ? a.posAgeSec : a.ageSec;
                    break;
                }
            }
        }
    } else fromMarkers(v, seen, dect2::markersFromAdsb(rx.rx.adsb), rx.rx.adsb.timeSec);
    fromMarkers(v, seen, dect2::markersFromAis(rx.rx.ais), rx.rx.ais.timeSec);
    fromMarkers(v, seen, dect2::markersFromAprs(rx.rx.packet), rx.rx.packet.timeSec);
    fromMarkers(v, seen, dect2::markersFromSonde(rx.rx.sonde), rx.rx.sonde.streamTimeS);

    dect2::AcarsTelemetry acars;
    if (rx.engine.acars().telemetry(acars, 0)) {
        for (const auto& a : acars.aircraft) {
            if (!a.hasPos) continue;
            Track t; t.kind = "aircraft"; t.sub = "ACARS"; t.src = "acars";
            t.id = aircraftMapId(a.icao, a.flight, a.reg.empty() ? a.flight : a.reg);
            if (t.id.empty()) continue;
            t.label = a.flight.empty() ? t.id : a.flight;
            t.lat = a.lat; t.lon = a.lon; t.age = acars.nowSec - a.lastHeardSec;
            t.hasHeard = true; t.heardAt = a.lastHeardSec;
            if (a.hasAlt) { t.hasAlt = true; t.alt = a.altFt; }
            if (a.hasTrack && a.hasSpeed) { t.hasHdg = true; t.hdg = a.trackDeg; }
            if (a.hasSpeed) { t.hasSpd = true; t.spd = a.speedKt; }
            applyKin(t, acarsKinematics(a, acars.nowSec), a.hasSpeed, a.hasTrack && a.hasSpeed);
            t.cat = a.posKind;
            for (const auto& p : a.track) t.path.push_back({p.lat, p.lon});
            addTrack(v, seen, std::move(t));
        }
    }
    dect2::AeroTelemetry aero;
    if (rx.engine.aero().telemetry(aero, 0)) {
        for (const auto& a : aero.aircraft) {
            if (!a.hasPos) continue;
            Track t; t.kind = "aircraft"; t.sub = "Inmarsat Aero"; t.src = "sat";
            t.id = a.aesId ? [&] { char b[16]; snprintf(b, sizeof b, "%06X", a.aesId); return std::string(b); }() : a.registration;
            t.label = a.flight.empty() ? (a.registration.empty() ? t.id : a.registration) : a.flight;
            t.lat = a.lat; t.lon = a.lon; t.age = aero.timeSec - a.posTime;
            t.hasHeard = true; t.heardAt = a.posTime;
            if (a.hasAlt) { t.hasAlt = true; t.alt = a.altFt; }
            if (a.hasTrack) { t.hasHdg = true; t.hdg = a.trackDeg; }
            if (a.hasSpeed) { t.hasSpd = true; t.spd = a.speedKt; }
            t.cat = a.posKind;
            for (const auto& p : a.track) t.path.push_back({p.lat, p.lon});
            addTrack(v, seen, std::move(t));
        }
    }
    dect2::HfdlTelemetry hfdl;
    if (rx.engine.hfdl().telemetry(hfdl, 0)) {
        for (const auto& a : hfdl.aircraft) {
            if (!a.hasPos) continue;
            Track t; t.kind = "aircraft"; t.sub = "HFDL"; t.src = "hfdl";
            const uint32_t ic = a.icao ? a.icao : (a.key < 0x1000000u ? a.key : 0u);
            t.id = aircraftMapId(ic, a.flight, a.reg);
            if (t.id.empty()) continue;
            t.label = a.flight.empty() ? t.id : a.flight; t.lat = a.lat; t.lon = a.lon; t.age = hfdl.nowSec - a.lastHeardSec;
            t.hasHeard = true; t.heardAt = a.lastHeardSec;
            { double la, lo, ag; if (hfdlEstimatedPosition(a.icao ? a.icao : a.key, hfdl.nowSec, la, lo, ag)) { t.lat = la; t.lon = lo; t.est = true; t.age = 0; t.heardAt = hfdl.nowSec; } }
            if (a.hasAlt) { t.hasAlt = true; t.alt = a.altFt; }
            applyKin(t, hfdlKinematics(a, hfdl.nowSec), false, false);
            t.cat = a.posKind;
            for (const auto& p : a.track) t.path.push_back({p.lat, p.lon});
            addTrack(v, seen, std::move(t));
        }
    }
    dect2::Vdl2Telemetry vdl;
    if (rx.engine.vdl2().telemetry(vdl, 0)) {
        for (const auto& a : vdl.aircraft) {
            if (!a.hasPos) continue;
            Track t; t.kind = "aircraft"; t.sub = "VDL2"; t.src = "acars";
            t.id = aircraftMapId(a.icao, a.flight, a.reg);
            if (t.id.empty()) continue;
            t.label = a.flight.empty() ? t.id : a.flight; t.lat = a.lat; t.lon = a.lon; t.age = vdl.nowSec - a.lastHeardSec;
            t.hasHeard = true; t.heardAt = a.lastHeardSec;
            { double la, lo, ag; if (vdl2EstimatedPosition(a.icao, vdl.nowSec, la, lo, ag)) { t.lat = la; t.lon = lo; t.est = true; t.age = 0; t.heardAt = vdl.nowSec; } }
            if (a.hasAlt) { t.hasAlt = true; t.alt = a.altFt; }
            if (a.hasKin) {   // ADS-C earth reference: measured
                t.hasSpd = true; t.spd = a.groundKt;
                if (a.hasTrack) { t.hasHdg = true; t.hdg = a.trackDeg; }
                t.hasVrate = true; t.vrate = a.vsFpm;
            }
            applyKin(t, vdl2Kinematics(a, vdl.nowSec), a.hasKin, a.hasKin && a.hasTrack);
            t.cat = a.posKind;
            for (const auto& p : a.track) t.path.push_back({p.lat, p.lon});
            addTrack(v, seen, std::move(t));
        }
    }
    dect2::IridiumTelemetry irdm;
    if (rx.engine.iridium().telemetry(irdm, 0)) {      // ACARS carried in Iridium short burst data: positions from the aircraft's own reports
        for (const auto& a : irdm.aircraft) {
            if (!a.hasPos) continue;
            Track t; t.kind = "aircraft"; t.sub = "Iridium ACARS"; t.src = "acars";
            t.id = aircraftMapId(a.icao, a.flight, a.reg);
            if (t.id.empty()) continue;
            t.label = a.flight.empty() ? (a.reg.empty() ? t.id : a.reg) : a.flight;
            t.lat = a.lat; t.lon = a.lon; t.age = irdm.timeSec - a.posTime;
            t.hasHeard = true; t.heardAt = a.posTime;
            if (a.hasAlt) { t.hasAlt = true; t.alt = a.altFt; }
            if (a.hasTrack) { t.hasHdg = true; t.hdg = a.trackDeg; }
            if (a.hasSpeed) { t.hasSpd = true; t.spd = a.speedKt; }
            t.cat = a.posKind;
            for (const auto& p : a.track) t.path.push_back({p.first, p.second});
            addTrack(v, seen, std::move(t));
        }
    }
    dect2::FlarmTelemetry flarm;
    if (rx.engine.flarm().telemetry(flarm, 0)) {
        for (const auto& a : flarm.aircraft) {
            if (!a.hasPos) continue;
            Track t; t.kind = "flarm"; t.sub = "FLARM"; t.src = "flarm";
            char id[16]; snprintf(id, sizeof id, "%06X", a.id & 0xFFFFFF);
            t.id = id; t.label = dect2::flarm::aircraftTypeName(a.acType);
            t.lat = a.lat; t.lon = a.lon; t.age = a.ageSec;
            t.hasHeard = true; t.heardAt = flarm.timeSec - a.ageSec;
            t.hasAlt = true; t.alt = a.altM / 0.3048;
            t.hasHdg = true; t.hdg = a.trackDeg; t.hasSpd = true; t.spd = a.speedKmh / 1.852;
            t.cat = dect2::flarm::aircraftTypeName(a.acType);
            for (const auto& p : a.track) t.path.push_back({p.lat, p.lon});
            addTrack(v, seen, std::move(t));
        }
    }

    {   // GNSS satellites decoded by this receiver: sub-points from the broadcast ephemeris, nothing from an almanac
        const dect2::GnssTelemetry& g = rx.rx.gnss;
        if (rx.rx.standard == 13) for (const dect2::GnssSatPos& s : g.sats) {
            dect2::GnssMapRow r;
            if (!dect2::gnssMapRow(s, r)) continue;
            Track t; t.kind = "gnss"; t.src = "gnss";
            t.id = r.id; t.label = r.id; t.sub = r.sub; t.extra = r.extra;
            t.lat = r.lat; t.lon = r.lon; t.hasAlt = true; t.alt = r.altKm;
            t.hasHeard = true; t.heardAt = g.signalSecs;   // grows while the receiver runs, so every report counts as heard again
            addTrack(v, seen, std::move(t));
        }
    }

    for (const dect2::OrbcommSat& s : rx.rx.orbcomm.sats) {
        if (!s.hasPos) continue;
        Track t; t.kind = "sat"; t.sub = "Orbcomm"; t.src = "sat";
        t.id = std::to_string(s.id); t.label = t.id;
        t.lat = s.lat; t.lon = s.lon; t.hasAlt = true; t.alt = s.altKm;
        t.hasHeard = true; t.heardAt = s.posTime > 0 ? s.posTime : s.lastHeard;
        if (rx.rx.orbcomm.timeSec > 0 && t.heardAt > 0) t.age = rx.rx.orbcomm.timeSec - t.heardAt;
        addTrack(v, seen, std::move(t));
    }

    for (const dect2::IridiumSatInfo& s : rx.rx.iridium.sats) {
        if (!s.hasPos || s.altKm < 100) continue;   // IRA sub-point of the satellite, not a beam centre on the ground
        Track t; t.kind = "sat"; t.sub = "Iridium"; t.src = "sat";
        t.id = std::to_string(s.id); t.label = t.id;
        t.lat = s.lat; t.lon = s.lon; t.hasAlt = true; t.alt = s.altKm;
        t.hasHeard = true; t.heardAt = s.lastHeard;
        if (rx.rx.iridium.timeSec > 0 && t.heardAt > 0) t.age = rx.rx.iridium.timeSec - t.heardAt;
        addTrack(v, seen, std::move(t));
    }

    for (const dect2::RidDrone& d : rx.rx.rid.drones) {
        if (!d.hasPos) continue;   // not the operator location
        Track t; t.kind = "drone"; t.sub = "Remote ID"; t.src = "rid";
        t.id = !d.serial.empty() ? d.serial : d.mac;
        t.label = t.id;
        t.lat = d.lat; t.lon = d.lon;
        t.hasHeard = true; t.heardAt = d.lastSec;
        if (rx.rx.rid.timeSec > 0 && t.heardAt > 0) t.age = rx.rx.rid.timeSec - t.heardAt;
        if (d.altGeo > -1000) { t.hasAlt = true; t.alt = d.altGeo; }
        if (d.direction >= 0) { t.hasHdg = true; t.hdg = d.direction; }
        if (d.speed >= 0) { t.hasSpd = true; t.spd = d.speed * 1.943844; }
        addTrack(v, seen, std::move(t));
    }

    for (const dect2::DjiDrone& d : rx.rx.dji.drones) {
        const double la = d.last.lat(), lo = d.last.lon();
        if (!validPos(la, lo)) continue;   // drone position only, not home or the pilot
        Track t; t.kind = "drone"; t.sub = "DJI"; t.src = "dji";
        t.id = d.serial.empty() ? "dji" : d.serial; t.label = t.id;
        t.lat = la; t.lon = lo; t.hasAlt = true; t.alt = d.last.altitudeM();
        t.hasHeard = true; t.heardAt = d.lastSec;
        if (rx.rx.dji.timeSec > 0 && t.heardAt > 0) t.age = rx.rx.dji.timeSec - t.heardAt;
        addTrack(v, seen, std::move(t));
    }

    for (const dect2::DscCall& c : rx.rx.marine.dsc) {
        if (!c.hasPos || !c.eccOk) continue;
        Track t; t.kind = "dsc"; t.sub = "DSC"; t.src = "dsc";
        const std::string raw = !c.fromMmsi.empty() ? c.fromMmsi : c.distressMmsi;
        const std::string mmsi = normalizeMmsiId(raw);
        t.id = mmsi.empty() ? raw : mmsi;
        t.label = t.id; t.lat = c.lat; t.lon = c.lon;
        t.hasHeard = true; t.heardAt = c.rxSec;
        if (!c.natureName.empty()) t.extra = c.natureName;
        addTrack(v, seen, std::move(t));
    }

    const dect2::DatawellTelemetry& dw = rx.rx.datawell;
    if (dw.sys.havePos && validPos(dw.sys.lat, dw.sys.lon)) {
        Track t; t.kind = "buoy"; t.sub = "Datawell"; t.src = "buoy";
        t.id = !dw.channel.empty() ? dw.channel : "datawell";
        t.label = t.id; t.lat = dw.sys.lat; t.lon = dw.sys.lon;
        t.hasHeard = true; t.heardAt = dw.timeSec;
        t.age = 0;
        addTrack(v, seen, std::move(t));
    }
    g.primed = true;
    gG = nullptr;
}

// The interesting ACARS messages of each aircraft track that were not sent before, redacted (phone numbers, e-mail), the newest few.
// Only with the user's choice (Server > share ACARS messages). Remembered by message time so a message goes up once.
void attachMsgs(std::vector<Track>& tracks) {
    static std::unordered_map<std::string, int64_t> sent;    // track id > time of the newest message sent
    std::unordered_map<std::string, int> holders;
    for (const Track& t : tracks) if (t.kind && !std::strcmp(t.kind, "aircraft") && !t.label.empty() && t.label != t.id) holders[dect2::acarsNormFlight(t.label)]++;
    for (Track& t : tracks) {
        if (!t.kind || std::strcmp(t.kind, "aircraft")) continue;
        const auto h = holders.find(dect2::acarsNormFlight(t.label));
        std::vector<dect2::AcarsLogEntry> v = acarslog::forTrack(t.id, t.label == t.id ? std::string() : t.label, h == holders.end() ? 1 : h->second);
        const int64_t from = sent.count(t.id) ? sent[t.id] : 0;
        int64_t newest = from;
        for (auto it = v.rbegin(); it != v.rend(); ++it) {      // oldest first
            if (it->wall <= from || !dect2::acarsInteresting(it->cat)) continue;
            dect2::AcarsLogEntry e = *it;
            e.text = dect2::acarsRedact(e.text);
            if (e.text.empty()) continue;
            roomsFeedText(e.text);
            newest = std::max(newest, e.wall);
            t.msgs.push_back(std::move(e));
        }
        if (t.msgs.size() > 5) t.msgs.erase(t.msgs.begin(), t.msgs.end() - 5);
        if (newest > from) sent[t.id] = newest;
    }
    if (sent.size() > 3000) sent.clear();
}

// What was said on the radio to an aircraft (airband transcripts, transcribe/transcribe_app.h), as messages of mode VOICE, not sent before. Only with the
// user's own choice (Server > also share transcripts), only airband, only text the speech engine was fairly sure of; phone numbers are removed.
void attachVoice(std::vector<Track>& tracks) {
    static std::unordered_map<std::string, int64_t> sent;    // track id > time of the newest transcript sent
    std::unordered_map<std::string, int> holders;
    for (const Track& t : tracks) if (t.kind && !std::strcmp(t.kind, "aircraft") && !t.label.empty() && t.label != t.id) holders[dect2::acarsNormFlight(t.label)]++;
    for (Track& t : tracks) {
        if (!t.kind || std::strcmp(t.kind, "aircraft")) continue;
        const auto h = holders.find(dect2::acarsNormFlight(t.label));
        std::vector<dect2::AcarsLogEntry> v = tx::voiceForTrack(t.id, t.label == t.id ? std::string() : t.label, h == holders.end() ? 1 : h->second);
        const int64_t from = sent.count(t.id) ? sent[t.id] : 0;
        int64_t newest = from;
        int n = 0;
        for (auto it = v.rbegin(); it != v.rend(); ++it) {      // oldest first
            if (it->wall <= from) continue;
            dect2::AcarsLogEntry e = *it;
            e.text = dect2::acarsRedact(e.text);
            if (e.text.empty()) continue;
            newest = std::max(newest, e.wall);
            if (n++ < 3) t.msgs.push_back(std::move(e));   // a few per aircraft per upload, together with the ACARS ones the server takes five
        }
        if (newest > from) sent[t.id] = newest;
    }
    if (sent.size() > 3000) sent.clear();
}

std::string buildJson(const std::string& station, const std::vector<Track>& tracks, const std::string& byName = std::string()) {
    std::string j = "{\"station\":\"" + jsonEsc(station) + "\"";
    if (!byName.empty()) j += ",\"by\":\"" + jsonEsc(byName) + "\"";   // optional public name, only when the user applied one; old servers ignore it
    j += ",\"tracks\":[";
    bool first = true;
    for (const Track& t : tracks) {
        if (!first) j += ',';
        first = false;
        j += "{\"kind\":\""; j += t.kind; j += "\",\"id\":\""; j += jsonEsc(t.id);
        if (t.kind && std::strcmp(t.kind, "aircraft") == 0 && isIcaoHexId(t.id)) {
            j += "\",\"icao\":\""; j += jsonEsc(t.id);
        }
        if (t.kind && (std::strcmp(t.kind, "vessel") == 0 || std::strcmp(t.kind, "dsc") == 0) && isMmsiId(t.id)) {
            j += "\",\"mmsi\":\""; j += jsonEsc(t.id);
        }
        j += "\",\"label\":\""; j += jsonEsc(t.label.empty() ? t.id : t.label);
        j += "\",\"sub\":\""; j += jsonEsc(t.sub);
        char b[96];
        snprintf(b, sizeof b, "\",\"lat\":%.6f,\"lon\":%.6f", t.lat, t.lon);
        j += b;
        if (t.hasAlt) { snprintf(b, sizeof b, ",\"alt\":%.2f", t.alt); j += b; }
        if (t.hasHdg) { snprintf(b, sizeof b, ",\"heading\":%.1f", t.hdg); j += b; }
        if (t.hasSpd) { snprintf(b, sizeof b, ",\"speed\":%.2f", t.spd); j += b; }
        if (t.hasVrate) { snprintf(b, sizeof b, ",\"vrate\":%.0f", t.vrate); j += b; }
        if (t.hasSquawk) { snprintf(b, sizeof b, ",\"squawk\":\"%04d\"", t.squawk); j += b; }
        snprintf(b, sizeof b, ",\"age\":%.1f", t.age); j += b;
        if (t.dfFreq > 0) { snprintf(b, sizeof b, ",\"freq\":%.0f,\"sigma\":%.1f,\"conf\":%.2f", t.dfFreq, t.dfSigma, t.dfConf); j += b; }
        if (t.est) j += ",\"est\":1";
        if (t.dv && (t.hasSpd || t.hasHdg || t.hasVrate)) j += ",\"dv\":1";
        if (!t.src.empty()) { j += ",\"src\":\""; j += jsonEsc(t.src); j += '"'; }
        if (!t.cat.empty()) { j += ",\"cat\":\""; j += jsonEsc(t.cat); j += '"'; }
        if (!t.extra.empty()) { j += ",\"extra\":\""; j += jsonEsc(t.extra); j += '"'; }
        if (!t.path.empty()) {
            j += ",\"path\":[";
            size_t n = 0;
            const size_t start = t.path.size() > 60 ? t.path.size() - 60 : 0;
            for (size_t i = start; i < t.path.size(); i++) {
                if (n++) j += ',';
                snprintf(b, sizeof b, "[%.5f,%.5f]", t.path[i].first, t.path[i].second);
                j += b;
            }
            j += ']';
        }
        if (!t.msgs.empty()) {
            j += ",\"msgs\":[";
            for (size_t i = 0; i < t.msgs.size(); i++) { if (i) j += ','; j += dect2::acarsEntryJson(t.msgs[i]); }
            j += ']';
        }
        j += '}';
    }
    j += "]}";
    return j;
}

void startPost(const std::string& url, const std::string& body, int n) {
    if (gBusy.exchange(true)) return;
    std::lock_guard<std::mutex> lk(gThMu);
    if (gTh.joinable()) gTh.join();
    const std::string ua = std::string("OnAir/") + ONAIR_VERSION + " (map ingest)";
    gTh = dect2::namedThread("onair-mapsend", [url, body, ua, n] {
        const bool ok = plat::postJson(url, body, ua);
        const int64_t t = nowMs();
        gLastTryMs.store(t, std::memory_order_relaxed);
        if (ok) {
            const int prevFail = gFails.load(std::memory_order_relaxed);
            gPhase.store(kOk, std::memory_order_relaxed);
            gLastN.store(n, std::memory_order_relaxed);
            gTotalN.fetch_add((uint64_t)std::max(0, n), std::memory_order_relaxed);
            gLastOkMs.store(t, std::memory_order_relaxed);
            gFails.store(0, std::memory_order_relaxed);
            if (prevFail > 0) mapLog("connection restored after %d failed ingest(s)", prevFail);
        } else {
            const int nfail = gFails.fetch_add(1, std::memory_order_relaxed) + 1;
            const int64_t lastOk = gLastOkMs.load(std::memory_order_relaxed);
            const bool outage = nfail >= kFailPopupAfter || lastOk == 0 || (t - lastOk) > 20000;
            if (outage) {
                gPhase.store(kRetry, std::memory_order_relaxed);
                mapLog("retrying after ingest failure #%d (last ok %s)", nfail, lastOk ? "earlier" : "never");
                if (nfail == kFailPopupAfter || lastOk == 0) gNeedRetryPopup.store(true, std::memory_order_relaxed);
            }
        }
        mapLog("ingest %s tracks=%d body=%zu B url=%s", ok ? "ok" : "failed", n, body.size(), url.c_str());
        fprintf(stderr, "map ingest %s n=%d (%zu bytes) %s\n", ok ? "ok" : "failed", n, body.size(), url.c_str());
        gBusy = false;
    });
}

void beginSend(App& s) {
    ensureStation(s);
    s.mapSend = true;
    savePrefs(s);
    gPhase = kConnecting;
    gAskConnecting = true;
    gToastRetry = false;
    gFails.store(0, std::memory_order_relaxed);
    gNeedRetryPopup.store(false, std::memory_order_relaxed);
    gPingNow = true;
    mapLog("send enabled origin=%s station=%s", originOf(s).c_str(), s.mapStationId.c_str());
}

void forgetHeard() {
    for (auto& kv : gRun) { kv.second.heard.clear(); kv.second.primed = false; }
}

void stopSend(App& s) {
    s.mapSend = false;
    savePrefs(s);
    gPhase = kOff;
    gAskConnecting = false;
    gToastRetry = false;
    gNeedRetryPopup.store(false, std::memory_order_relaxed);
    gPingNow = false;
    mapLog("send disabled");
}

// "Your name on the map": optional and public. Nothing is sent until the user has typed a name and pressed Use this name.
// Never filled in from the OS user name. The preview line shows exactly what goes out.
void nameEditor(App& s) {
    static char buf[40] = {0};
    static bool init = false;
    if (!init) { std::snprintf(buf, sizeof buf, "%s", s.mapName.c_str()); init = true; }
    ImGui::PushTextWrapPos(380 * gUi);
    ImGui::TextUnformatted(tr("Your name on the map (optional)"));
    ImGui::InputTextWithHint("##mapname", tr("empty = anonymous"), buf, sizeof buf);
    std::string name;
    const int r = mapname::check(buf, name);
    if (r == 0) ImGui::TextDisabled(tr("This is what will be sent: name = %s"), name.empty() ? tr("(none, anonymous)") : name.c_str());
    else {
        const char* why = r == 1 ? tr("Use 2 to 24 characters.") : r == 2 ? tr("Use only letters, digits, spaces and _ - . in the name.")
                        : r == 3 ? tr("The name cannot be a web address or e-mail address.") : tr("That name is not allowed. Please choose another.");
        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.3f, 1.f), "%s", why);
    }
    ImGui::TextDisabled("%s", tr("This name is public. It is shown on every icon you were the last to hear, and people can search for it on the map. Leave it empty to stay anonymous."));
    ImGui::PopTextWrapPos();
    if (r != 0) ImGui::BeginDisabled(true);
    if (ImGui::Button(tr("Use this name")) && r == 0) { s.mapName = name; savePrefs(s); }
    if (r != 0) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Stay anonymous"))) { buf[0] = 0; s.mapName.clear(); savePrefs(s); }
    if (!s.mapName.empty()) ImGui::TextDisabled(tr("Sending as: %s"), s.mapName.c_str());
}

// "Also share interesting ACARS messages": free text and crew / operations / company / maintenance messages from the aircraft on the map.
// They are public radio, but the text may name people, so phone numbers and e-mail addresses are removed before upload and this is its own choice.
void msgsToggle(App& s, bool menu) {
    bool v = s.mapMsgs;
    const char* label = tr("Also share interesting ACARS messages (text becomes public on the map)");
    bool hit = false;
    if (menu) hit = ImGui::MenuItem(label, nullptr, v);
    else hit = ImGui::Checkbox(label, &v) && v != s.mapMsgs;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Messages an aircraft sent by ACARS (free text, crew, operations, company and maintenance messages), shown on that aircraft's card on the public map for 48 hours. Phone numbers and e-mail addresses are removed first. Position reports and link tests are not part of this. You can turn it off at any time."));
    if (!hit) return;
    s.mapMsgs = menu ? !s.mapMsgs : v;
    savePrefs(s);
}

// "Also share GPS interference cells": per 0.5 degree cell, how many aircraft the ADS-B receiver heard and how many of them reported a degraded
// GPS position. No aircraft ids, no positions finer than the cell. Its own choice, off until chosen.
void jamToggle(App& s, bool menu) {
    bool v = s.mapJam;
    const char* label = tr("Also share GPS interference cells (counts per area, no aircraft)");
    bool hit = false;
    if (menu) hit = ImGui::MenuItem(label, nullptr, v);
    else hit = ImGui::Checkbox(label, &v) && v != s.mapJam;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("For every 0.5 degree square (about 55 km) around your ADS-B receiver: how many aircraft were heard in the last hour and how many of them reported a poor GPS position. Only these two numbers per square are sent, never aircraft identities, tracks or your location. It feeds the GPS interference layer of the public map and is an indicator, not proof of jamming. Off until you choose it."));
    if (!hit) return;
    s.mapJam = menu ? !s.mapJam : v;
    savePrefs(s);
}

// POST /jam every 5 minutes while sending and the option is on (its own thread, apart from the track posts)
void jamTick(App& s) {
    static int64_t lastTry = 0;
    static std::atomic<bool> busy{false};
    const int64_t now = nowMs();
    if (!s.mapJam || busy.load() || (lastTry != 0 && now - lastTry < 300000)) return;
    ensureStation(s);
    const std::string body = adsbJamBody(s.mapStationId);
    lastTry = now;
    if (body.empty()) return;
    busy = true;
    const std::string url = originOf(s) + "/jam", ua = std::string("OnAir/") + ONAIR_VERSION + " (map jam)";
    dect2::namedThread("onair-mapsend2", [url, body, ua] {
        const bool ok = plat::postJson(url, body, ua);
        mapLog("jam %s body=%zu B", ok ? "ok" : "failed", body.size());
        busy = false;
    }).detach();   // short-lived, touches only its own copies and the flag
}

// "Also share transcripts": what the airband voice transcripts say to or about an aircraft, on that aircraft's card. Public radio, but the text may be wrong
// or name people, so this is its own choice and off until chosen. Only airband; nothing from marine, ham or other private channels.
void voiceToggle(App& s, bool menu) {
    bool v = s.mapVoice;
    const char* label = tr("Also share transcripts of aircraft radio calls (text becomes public on the map)");
    bool hit = false;
    if (menu) hit = ImGui::MenuItem(label, nullptr, v);
    else hit = ImGui::Checkbox(label, &v) && v != s.mapVoice;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("The text of airband radio calls that a speech engine on this computer made from what you receive (Transcripts window), linked to the aircraft by the callsign spoken in it, shown on that aircraft's card on the public map for 48 hours. It can contain mistakes. Phone numbers are removed first. Only airband is shared, never marine, amateur or other channels. Off until you choose it, and you can turn it off at any time."));
    if (!hit) return;
    s.mapVoice = menu ? !s.mapVoice : v;
    savePrefs(s);
}


// ---- ham contacts (own choice, off until chosen; see core/include/dect2/ham/ham_mapfeed.h) ----
dect2::ham::mapfeed::Feed gHamFeed;
dect2::ham::mapfeed::Switches gHamSw;          // refreshed every tick from the settings
std::map<std::string, std::string> gHamGrids;  // callsign -> locator heard (own station included); bounded
bool gAskHamConsent = false;

struct TickSink : dect2::ham::mapfeed::Sink {   // adds the due contacts to this tick's ingest post as "ham" tracks
    std::vector<Track>* out = nullptr; bool online = true;
    bool post(const std::vector<dect2::ham::mapfeed::Track>& v) override {
        if (!online || !out) return false;
        for (const auto& h : v) {
            if (out->size() >= kMaxTracks) break;
            Track t; t.kind = "ham"; t.id = h.id; t.label = h.label; t.sub = h.sub; t.src = "ham"; t.extra = h.extra;
            t.lat = h.lat; t.lon = h.lon; t.age = h.ageSec;
            out->push_back(std::move(t));
        }
        return true;
    }
};

void hamToggle(App& s, bool menu) {
    bool v = s.mapHam;
    const char* label = tr("Share ham contacts on the map");
    bool hit = false;
    if (menu) hit = ImGui::MenuItem(label, nullptr, v);
    else hit = ImGui::Checkbox(label, &v) && v != s.mapHam;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Decoded FT8, FT4 and WSPR contacts and the QSOs you log: the callsigns, the mode, the band and the time, drawn at the centre of each station's locator square. Off until you choose it."));
    if (!hit) return;
    if (!s.mapHam && !s.mapHamConsent) { gAskHamConsent = true; return; }
    s.mapHam = !s.mapHam;
    if (!s.mapHam) gHamFeed.clear();
    savePrefs(s);
}

void hamPrecisionToggle(App& s, bool menu) {
    bool v = s.mapHam6;
    const char* label = tr("Ham contacts: use 6-character locators (about 5 x 2.5 km)");
    bool hit = menu ? ImGui::MenuItem(label, nullptr, v) : (ImGui::Checkbox(label, &v) && v != s.mapHam6);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("By default a contact is drawn at the centre of its 4-character square (about 110 x 55 km). This draws it at the centre of the 6-character square instead. Your own position is never sent more precisely than your locator."));
    if (!hit) return;
    s.mapHam6 = menu ? !s.mapHam6 : v;
    savePrefs(s);
}

void hamConsentPopup(App& a) {
    App& s = rootApp(a);
    if (gAskHamConsent) { ImGui::OpenPopup("##maphamconsent"); gAskHamConsent = false; }
    ImGui::SetNextWindowSizeConstraints(ImVec2(420 * gUi, 0), ImVec2(520 * gUi, 400 * gUi));
    if (!ImGui::BeginPopupModal("##maphamconsent", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::PushTextWrapPos(480 * gUi);
    ImGui::TextUnformatted(tr("When this is on, the amateur radio contacts you decode (FT8, FT4, WSPR) and the QSOs you log are sent to map.on-air.dev for the public map: the callsigns of both stations, the mode, the band and frequency, the time, and where each station is as the centre of its locator square (4 characters, about 110 x 55 km, unless you choose 6). Your own station is drawn at your locator square, never at an exact location. Logbook notes, signal reports, your QRZ or other account details and your receiver location are not sent. Callsigns are public information, but this is your choice: it needs \"Send data to map server\" to be on as well, repeats are sent once, and you can turn it off at any time in the Server menu."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 8 * gUi));
    if (ImGui::Button(tr("Share ham contacts"), ImVec2(190 * gUi, 0))) {
        s.mapHamConsent = true; s.mapHam = true; savePrefs(s);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Not now"), ImVec2(100 * gUi, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void consentPopup(App& a) {
    App& s = rootApp(a);
    if (gAskConsent) { ImGui::OpenPopup("##mapconsent"); gAskConsent = false; }
    ImGui::SetNextWindowSizeConstraints(ImVec2(420 * gUi, 0), ImVec2(520 * gUi, 400 * gUi));
    if (!ImGui::BeginPopupModal("##mapconsent", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::PushTextWrapPos(480 * gUi);
    ImGui::TextUnformatted(tr("When this is on, positions decoded from the signals you receive are sent to map.on-air.dev so they can appear on the public map. Catalogue satellite orbits, your receiver location, drone operator locations, privacy-flagged FLARM aircraft, and the built-in test signal are not sent. Your name is not sent unless you choose to type one under Server > Your name on the map; if you do, it is public, shown on the icons you were the last to hear, and searchable. You can turn this off at any time from Server."));
    ImGui::Dummy(ImVec2(0, 4 * gUi));
    ImGui::TextUnformatted(tr("Messages that aircraft send by ACARS (free text and crew, operations, company and maintenance messages) can be shown on the aircraft's card as well, with phone numbers and e-mail addresses removed. This is a separate choice that you can change in the Server menu."));
    static bool shareMsgs = true;
    ImGui::Checkbox(tr("Also share interesting ACARS messages (text becomes public on the map)"), &shareMsgs);
    ImGui::Dummy(ImVec2(0, 4 * gUi));
    ImGui::TextUnformatted(tr("OnAir asks this question at every start. \"No, do not ask again\" stops it until OnAir is updated."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 8 * gUi));
    const bool wasOn = s.mapSend && s.mapConsent;   // sending was already on from an earlier answer
    const char* sendLbl = wasOn ? tr("Keep sending positions") : tr("Send positions");
    if (ImGui::Button(sendLbl, fitButton(sendLbl, 190 * gUi))) {
        ensureStation(s);
        s.mapConsent = true;
        s.mapMsgs = shareMsgs;
        s.mapDeclined = false;
        if (s.mapSend) savePrefs(s); else beginSend(s);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Not now"), fitButton(tr("Not now"), 100 * gUi))) {
        stopSend(s);   // no connecting in this session; the question comes again at the next start
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Nothing is sent in this session. OnAir asks again the next time it starts."));
    ImGui::SameLine();
    if (ImGui::Button(tr("No, do not ask again"), fitButton(tr("No, do not ask again"), 190 * gUi))) {
        stopSend(s);
        s.mapDeclined = true;
        s.mapDeclinedVersion = ONAIR_VERSION;
        savePrefs(s);
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Nothing is sent and this question is not shown again until OnAir is updated. You can still turn it on later in the Server menu."));
    ImGui::EndPopup();
}

void mapToast(const char* text) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) return;
    const ImVec2 sz = ImGui::CalcTextSize(text);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 72 * gUi), ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(sz.x + 36 * gUi, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.10f, 0.94f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4 * gUi);
    if (ImGui::Begin("##maptoast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::TextUnformatted(text);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void connectingPopup() {
    if (gAskConnecting) gAskConnecting = false;
    if (gPhase.load(std::memory_order_relaxed) == kConnecting) mapToast(tr("Connecting to server…"));
}

void retryPopup() {
    const int ph = gPhase.load(std::memory_order_relaxed);
    if (gNeedRetryPopup.exchange(false)) gToastRetry = true;
    if (ph == kOk || ph == kOff) gToastRetry = false;
    if (gToastRetry && ph == kRetry) mapToast(tr("Retrying connecting with server"));
}

void txOffToast() {
    if (gToastTxUntil == 0 || nowMs() >= gToastTxUntil) return;
    mapToast(tr("The connection to the map server has been turned off. It cannot be turned back on while you are transmitting."));
}

// The first thing a new user sees: a question whether to take part in the public map, because few people find Server > Send data on their own.
// It asks at every start, also after consent was given, until "No, do not ask again" is chosen; that lasts until the next update of OnAir
// (another ONAIR_VERSION). "Not now" turns sending off for the session only. Never while transmitting.
void startupQuestion(App& a) {
    static bool asked = false;
    if (asked) return;
    if (a.specDemo) { asked = true; return; }   // the --specdemo screenshot runs: no question over the picture, nothing sent
    if (getenv("DECT2_SKIP_FIRSTRUN")) { asked = true; return; }   // dev / screenshots (--no-first-run): nothing covers the UI
    App& s = rootApp(a);
    if (s.mapDeclined && s.mapDeclinedVersion != ONAIR_VERSION) { s.mapDeclined = false; s.mapDeclinedVersion.clear(); savePrefs(s); }   // an update: ask again
    if (!mapShouldAsk(s.mapConsent, s.mapDeclined, s.mapDeclinedVersion, ONAIR_VERSION, gTxHold || txActive())) { asked = true; return; }
    if (ImGui::GetFrameCount() < 90) return;   // after the first frames: the window is up and the layout is settled
    asked = true;
    gAskConsent = true;
}

void mapPopups(App& a) {
    roomsPopups(rootApp(a));
    startupQuestion(a);
    consentPopup(a);
    hamConsentPopup(a);
    connectingPopup();
    retryPopup();
    txOffToast();
}

void sendToggle(App& a, bool menu) {
    App& s = rootApp(a);
    if (gTxHold || txActive()) {
        ImGui::BeginDisabled(true);
        if (menu) ImGui::MenuItem(tr("Send data to map server"), nullptr, false);
        else { bool v = false; ImGui::Checkbox(tr("Send data to map server"), &v); }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tr("Turned off while transmitting, so a signal from this radio is not sent to the map."));
        return;
    }
    const bool on = s.mapSend;
    bool hit = false;
    if (menu) hit = ImGui::MenuItem(tr("Send data to map server"), nullptr, on);
    else {
        bool v = on;
        hit = ImGui::Checkbox(tr("Send data to map server"), &v) && v != on;
    }
    if (!hit) return;
    if (!on) {
        if (!s.mapConsent) gAskConsent = true;
        else beginSend(s);
    } else stopSend(s);
}

} // namespace

void mapSendServerMenu(App& a) {
    if (!ImGui::BeginMenu(tr("Server"))) { mapPopups(a); return; }
    sendToggle(a, true);
    if (rootApp(a).mapSend) msgsToggle(rootApp(a), true);
    if (rootApp(a).mapSend) voiceToggle(rootApp(a), true);
    if (rootApp(a).mapSend) jamToggle(rootApp(a), true);
    if (rootApp(a).mapSend) hamToggle(rootApp(a), true);
    if (rootApp(a).mapSend && rootApp(a).mapHam) hamPrecisionToggle(rootApp(a), true);
    if (rootApp(a).mapSend && ImGui::BeginMenu(tr("Your name on the map"))) { nameEditor(rootApp(a)); ImGui::EndMenu(); }
    ImGui::Separator();
    roomsMenu(rootApp(a), true);
    ImGui::TextDisabled("%s", tr("The map is at map.on-air.dev"));
    ImGui::EndMenu();
    mapPopups(a);
}

void mapSendButton(App& a) {
    if (ImGui::SmallButton(tr("Server"))) ImGui::OpenPopup("##serverpop");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Send decoded positions to the public map"));
    if (ImGui::BeginPopup("##serverpop")) {
        sendToggle(a, false);
        if (rootApp(a).mapSend) msgsToggle(rootApp(a), false);
        if (rootApp(a).mapSend) voiceToggle(rootApp(a), false);
        if (rootApp(a).mapSend) jamToggle(rootApp(a), false);
        if (rootApp(a).mapSend) hamToggle(rootApp(a), false);
        if (rootApp(a).mapSend && rootApp(a).mapHam) hamPrecisionToggle(rootApp(a), false);
        if (rootApp(a).mapSend && ImGui::BeginMenu(tr("Your name on the map"))) { nameEditor(rootApp(a)); ImGui::EndMenu(); }
        ImGui::Separator();
        roomsMenu(rootApp(a), false);
        ImGui::TextDisabled("%s", tr("The map is at map.on-air.dev"));
        ImGui::EndPopup();
    }
    mapPopups(a);
}

bool mapFamilySends(int family) {
    switch (family) {
    case 11:  // ADS-B
    case 13:  // radiosonde
    case 14:  // AIS
    case 15:  // marine (DSC)
    case 16:  // ACARS
    case 18:  // Inmarsat Aero
    case 19:  // Iridium
    case 24:  // APRS
    case 27:  // VDL2
    case 35:  // HFDL
    case 44:  // Orbcomm
    case 56:  // Remote ID
    case 66:  // FLARM
    case 68:  // DJI
    case 74:  // Datawell
        return true;
    default: return false;
    }
}

std::string mapSendStatusLine() {
    const int ph = gPhase.load(std::memory_order_relaxed);
    if (ph == kOff) return {};
    char b[80];
    if (ph == kConnecting) return tr("connecting");
    if (ph == kRetry) return tr("retrying");
    const int n = gLastN.load(std::memory_order_relaxed);
    const uint64_t tot = gTotalN.load(std::memory_order_relaxed);
    if (n > 0) snprintf(b, sizeof b, "%s  %d sent", tr("connected"), n);
    else if (tot > 0) snprintf(b, sizeof b, "%s  %llu sent", tr("connected"), (unsigned long long)tot);
    else snprintf(b, sizeof b, "%s", tr("connected"));
    return b;
}

void mapHamRemember(const std::string& call, const std::string& grid) {
    if (call.empty() || grid.size() < 4) return;
    if (gHamGrids.size() > 2000) gHamGrids.clear();
    std::string c = call;
    for (char& ch : c) ch = (char)std::toupper((unsigned char)ch);
    gHamGrids[c] = grid;
}

void mapHamHeard(const char* mode, double rfHz, const std::string& from, const std::string& fromGrid, const std::string& to, double tUtc) {
    if (!dect2::ham::mapfeed::allowed(gHamSw) || !mode || from.empty() || to.empty()) return;   // off: not even remembered
    mapHamRemember(from, fromGrid);
    std::string f = from, t = to;
    for (char& ch : f) ch = (char)std::toupper((unsigned char)ch);
    for (char& ch : t) ch = (char)std::toupper((unsigned char)ch);
    const auto fi = gHamGrids.find(f), ti = gHamGrids.find(t);
    dect2::ham::mapfeed::Contact c;
    c.mode = mode; c.freqHz = rfHz; c.from = f; c.to = t; c.tUtc = tUtc;
    if (fi == gHamGrids.end()) return;   // no locator for the station: no position, no track
    c.fromGrid = fi->second;
    if (ti != gHamGrids.end()) c.toGrid = ti->second;
    gHamFeed.offer(gHamSw, c, (double)std::time(nullptr));
}

void mapSendTick(App& a) {
    App& s = rootApp(a);
    mapPopups(a);
    gHamSw = {s.mapSend, s.mapConsent, s.mapHam, s.mapHamConsent};
    if (txActive() || gTxHold) {
        if (s.mapSend) mapPauseForTransmit(s);
        if (txActive()) return;
        gTxHold = false;
        gTxPausedSend = false;
        gToastTxUntil = 0;
        forgetHeard();
        return;
    }
    if (!s.mapSend || !s.mapConsent) {
        if (gPhase.load(std::memory_order_relaxed) != kOff && !s.mapSend) gPhase = kOff;
        return;
    }
    if (gPhase.load(std::memory_order_relaxed) == kOff) gPhase = kConnecting;
    jamTick(s);
    if (gBusy) return;
    ensureStation(s);
    const int64_t now = nowMs();
    const int ph = gPhase.load(std::memory_order_relaxed);
    const int64_t lastOk = gLastOkMs.load(std::memory_order_relaxed);
    const int64_t lastTry = gLastTryMs.load(std::memory_order_relaxed);
    const bool needHb = lastOk == 0 || (now - lastOk) >= (int64_t)(kHeartbeatSec * 1000);
    const bool retryWait = (ph == kConnecting || ph == kRetry) && lastTry != 0 && (now - lastTry) < (int64_t)(kRetrySec * 1000);
    if (retryWait && !gPingNow) return;

    std::vector<Track> tracks;
    std::unordered_set<std::string> seen;
    for (App* r : receiversAll()) if (r) collectFrom(*r, tracks, seen);
    {   // the direction finder's bearing, only when the user chose to share it (View > Direction finder > Sharing)
        DfMapLine dl;
        if (dfMapLine(s, dl) && tracks.size() < kMaxTracks) {
            Track t; t.kind = "doa"; t.src = "df"; t.id = "df"; t.label = "DF"; t.lat = dl.lat; t.lon = dl.lon; t.age = dl.ageSec;
            t.hasHdg = true; t.hdg = dl.bearing; t.dfFreq = dl.freqHz; t.dfSigma = dl.sigma; t.dfConf = dl.conf;
            if (validPos(t.lat, t.lon)) tracks.push_back(std::move(t));
        }
    }
    if (s.mapMsgs) attachMsgs(tracks);
    {   // ham contacts: only with the master switch, its consent and the ham choice with its own consent; queued while the link is down
        gHamSw = {s.mapSend, s.mapConsent, s.mapHam, s.mapHamConsent};
        gHamFeed.limits().gridChars = s.mapHam6 ? 6 : 4;
        TickSink sink; sink.out = &tracks; sink.online = (ph != kRetry);
        gHamFeed.flush(gHamSw, sink, (double)std::time(nullptr));
    }
    if (s.mapVoice) attachVoice(tracks);
    const bool ping = gPingNow || needHb || (tracks.empty() && (ph == kConnecting || ph == kRetry));
    if (!gPingNow && !needHb && !tracks.empty()) {
        static int64_t lastSend = 0;
        if (lastSend != 0 && (now - lastSend) < (int64_t)(kMinInterval * 1000)) return;
        lastSend = now;
    } else if (!ping) return;
    gPingNow = false;
    startPost(originOf(s) + "/ingest", buildJson(s.mapStationId, tracks, s.mapName), (int)tracks.size());
}

bool mapPauseForTransmit(App& a) {
    App& s = rootApp(a);
    gTxHold = true;
    if (!s.mapSend || !s.mapConsent) return false;
    gTxPausedSend = true;
    stopSend(s);
    gToastTxUntil = nowMs() + 8000;
    mapLog("send paused for transmit");
    return true;
}

void mapTransmitAbort(App& a) {
    const bool restore = gTxPausedSend;
    gTxHold = false;
    gTxPausedSend = false;
    gToastTxUntil = 0;
    if (restore && rootApp(a).mapConsent) beginSend(rootApp(a));
}

void mapTransmitEnded() {
    gTxHold = false;
    gTxPausedSend = false;
    gToastTxUntil = 0;
    forgetHeard();
}

void mapSendShutdown() {
    roomsShutdown();
    std::thread t;
    { std::lock_guard<std::mutex> lk(gThMu); if (gTh.joinable()) t = std::move(gTh); }
    if (t.joinable()) t.join();
    gBusy = false;
}

std::string mapSendDiagnosticsBlock(const App& a) {
    App& s = rootApp(const_cast<App&>(a));
    const int ph = gPhase.load(std::memory_order_relaxed);
    const int64_t lastOk = gLastOkMs.load(std::memory_order_relaxed);
    const int64_t lastTry = gLastTryMs.load(std::memory_order_relaxed);
    const int fails = gFails.load(std::memory_order_relaxed);
    const int lastN = gLastN.load(std::memory_order_relaxed);
    const uint64_t tot = gTotalN.load(std::memory_order_relaxed);
    const int64_t now = nowMs();
    auto ago = [&](int64_t t) -> std::string {
        if (t <= 0) return "never";
        const double sec = (now - t) / 1000.0;
        if (sec < 2) return "just now";
        char b[48];
        snprintf(b, sizeof b, "%.1f s ago", sec);
        return b;
    };
    std::string out = "\nMap server connection:\n";
    out += "  enabled: " + std::string(s.mapSend && s.mapConsent ? "yes" : "no") + "\n";
    out += "  consent: " + std::string(s.mapConsent ? "yes" : "no") + "\n";
    out += "  origin: " + originOf(s) + "\n";
    out += "  station id: " + s.mapStationId + "\n";
    out += "  state: " + std::string(phaseName(ph)) + "\n";
    out += "  consecutive ingest failures: " + std::to_string(fails) + "\n";
    out += "  last successful ingest: " + ago(lastOk) + "\n";
    out += "  last ingest attempt: " + ago(lastTry) + "\n";
    out += "  last batch size: " + std::to_string(lastN) + " tracks\n";
    out += "  total tracks sent (session): " + std::to_string(tot) + "\n";
    out += "  ingest in progress: " + std::string(gBusy.load(std::memory_order_relaxed) ? "yes" : "no") + "\n";
    out += "\n  Connection log (newest last):\n";
    std::deque<std::string> logCopy;
    { std::lock_guard<std::mutex> lk(gMapLogMu); logCopy = gMapLog; }
    if (logCopy.empty()) out += "    (none yet)\n";
    else for (const std::string& line : logCopy) out += "    " + line + "\n";
    return out;
}
