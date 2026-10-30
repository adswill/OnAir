// Group 2 adapters: ACARS, VDL2, HFDL (aircraft that talk), UAT (aircraft), radiosondes. Same TrackBook rules as mode_adapters.cpp.
#include "dect2/acars/acars_tel.h"
#include "dect2/hfdl/hfdl_tel.h"
#include "dect2/session/mode_adapters.h"
#include "dect2/session/track_book.h"
#include "dect2/sonde/sonde_tel.h"
#include "dect2/uat/uat_tel.h"
#include "dect2/vdl2/vdl2_tel.h"
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>

namespace dect2::sess {
namespace {
std::mutex gMu;
std::map<std::pair<uint16_t, uint16_t>, std::unique_ptr<TrackBook>>& books() { static auto* m = new std::map<std::pair<uint16_t, uint16_t>, std::unique_ptr<TrackBook>>; return *m; }
TrackBook& book(uint16_t mode, uint16_t rx, int64_t gapMs, int64_t updateMs) {
    auto& b = books()[{mode, rx}];
    if (!b) { BookConfig c; c.mode = mode; c.receiver = rx; c.kind = Kind::Track; c.gapMs = gapMs; c.updateMs = updateMs; b.reset(new TrackBook(c)); }
    return *b;
}
std::string hex6(uint32_t v) { char b[16]; snprintf(b, sizeof b, "%06X", v & 0xFFFFFF); return b; }
// the wall time an aircraft was last heard, from the signal clock of the report
int64_t seenMs(int64_t now, double nowSec, double heardSec) { return now - (int64_t)(std::max(0.0, nowSec - heardSec) * 1000.0); }

// reg / flight / icao -> key and title
void idOf(Session& s, const char* what, const std::string& reg, const std::string& flight, uint32_t icao) {
    const std::string ic = icao ? hex6(icao) : std::string();
    const std::string main = !reg.empty() ? reg : !ic.empty() ? ic : flight;
    s.key = std::string(what) + " " + (!ic.empty() ? ic : main);
    s.who[0] = main;
    s.alias[0] = flight;
    s.title = main + (flight.empty() || flight == main ? "" : " (" + flight + ")");
}
}  // namespace

void observeAcars(const AcarsTelemetry& t, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    TrackBook& b = book(16, rx, 900000, 15000);
    const int64_t now = wallNowMs();
    for (const AcarsAircraft& a : t.aircraft) {
        if (a.reg.empty() && !a.icao) continue;
        Session s;
        idOf(s, "ACARS", a.reg, a.flight, a.icao);
        s.eventCount = a.messages;
        s.freqHz = (uint64_t)a.freqHz;
        s.levelDb = a.levelDb;
        if (a.hasPos) { s.lat = a.lat; s.lon = a.lon; }
        if (a.hasAlt) s.altM = (float)(a.altFt * 0.3048);
        b.observe(std::move(s), now, seenMs(now, t.nowSec, a.lastHeardSec));
    }
}

void observeVdl2(const Vdl2Telemetry& t, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    TrackBook& b = book(27, rx, 900000, 15000);
    const int64_t now = wallNowMs();
    for (const Vdl2Aircraft& a : t.aircraft) {
        Session s;
        idOf(s, "VDL2", a.reg, a.flight, a.icao);
        s.eventCount = a.messages;
        s.freqHz = (uint64_t)a.freqHz;
        s.levelDb = a.levelDb;
        if (a.hasPos) { s.lat = a.lat; s.lon = a.lon; }
        if (a.hasAlt) s.altM = (float)(a.altFt * 0.3048);
        b.observe(std::move(s), now, a.lastWall > 0 ? a.lastWall * 1000 : seenMs(now, t.nowSec, a.lastHeardSec));
    }
}

void observeHfdl(const HfdlTelemetry& t, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    TrackBook& b = book(35, rx, 1800000, 30000);
    const int64_t now = wallNowMs();
    for (const HfdlAircraft& a : t.aircraft) {
        Session s;
        idOf(s, "HFDL", a.reg, a.flight, a.icao);
        if (!a.icao && s.key == "HFDL ") s.key = "HFDL " + std::to_string(a.key);
        s.eventCount = a.messages;
        s.freqHz = (uint64_t)a.freqHz;
        s.levelDb = a.levelDb;
        if (a.hasPos) { s.lat = a.lat; s.lon = a.lon; }
        if (a.hasAlt) s.altM = (float)(a.altFt * 0.3048);
        b.observe(std::move(s), now, seenMs(now, t.nowSec, a.lastHeardSec));
    }
}

void observeUat(const UatTelemetry& t, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    TrackBook& b = book(83, rx, 120000, 5000);
    const int64_t now = wallNowMs();
    for (const UatAircraft& a : t.aircraft) {
        Session s;
        const std::string ic = hex6(a.icao);
        s.key = "ICAO " + ic;
        s.who[0] = ic;
        s.alias[0] = a.callsign;
        s.title = a.callsign.empty() ? "Aircraft " + ic : a.callsign + " (" + ic + ")";
        if (a.hasPos) { s.lat = a.lat; s.lon = a.lon; }
        if (a.hasAlt) s.altM = (float)(a.altFt * 0.3048);
        s.levelDb = a.levelDbfs;
        s.eventCount = a.messages;
        s.freqHz = 978000000ull;
        b.observe(std::move(s), now, now - (int64_t)(a.ageSec * 1000.f));
    }
}

void observeSonde(const SondeTelemetry& t, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    TrackBook& b = book(13, rx, 900000, 15000);
    const int64_t now = wallNowMs();
    for (const SondeInfo& a : t.sondes) {
        if (a.serial.empty()) continue;
        Session s;
        s.key = "sonde " + a.serial;
        s.who[0] = a.serial;
        s.alias[0] = a.type;
        s.title = a.type + " " + a.serial;
        if (a.hasPos) { s.lat = a.lat; s.lon = a.lon; s.altM = (float)a.altM; }
        s.snrDb = a.snrDb;
        s.eventCount = (uint32_t)std::min<uint64_t>(a.framesOk, 4000000000ull);
        s.freqHz = (uint64_t)a.freqHz;
        b.observe(std::move(s), now, now - (int64_t)(std::max(0.0, a.lastHeardS) * 1000.0));
    }
}

void adaptersEndAir(uint16_t mode, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    auto it = books().find({mode, rx});
    if (it != books().end() && it->second) it->second->endAll(wallNowMs());
}

}  // namespace dect2::sess
