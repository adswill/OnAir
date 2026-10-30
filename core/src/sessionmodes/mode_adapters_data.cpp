// Group 3 adapters: LoRa mesh and LoRaWAN, Iridium, Orbcomm, Inmarsat Aero and Inmarsat-C. Same TrackBook rules as mode_adapters.cpp: only queue
// pushes, message text is never stored here.
#include "dect2/aero/aero_tel.h"
#include "dect2/inmc/inmc_tel.h"
#include "dect2/iridium/iridium_tel.h"
#include "dect2/mesh/mesh_tel.h"
#include "dect2/orbcomm/orbcomm_tel.h"
#include "dect2/session/mode_adapters.h"
#include "dect2/session/track_book.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>

namespace dect2::sess {
namespace {
std::mutex gMu;
std::map<std::tuple<uint16_t, uint16_t, int>, std::unique_ptr<TrackBook>>& books() { static auto* m = new std::map<std::tuple<uint16_t, uint16_t, int>, std::unique_ptr<TrackBook>>; return *m; }
// slot lets one mode keep several books with different gaps (a satellite and an aircraft are not tracked alike)
TrackBook& book(uint16_t mode, uint16_t rx, int slot, Kind kind, int64_t gapMs, int64_t updateMs) {
    auto& b = books()[{mode, rx, slot}];
    if (!b) { BookConfig c; c.mode = mode; c.receiver = rx; c.kind = kind; c.gapMs = gapMs; c.updateMs = updateMs; b.reset(new TrackBook(c)); }
    return *b;
}
// when the thing was last heard: the report's wall time minus the signal seconds since
int64_t seenMs(int64_t now, double nowSec, double heardSec) { return now - (int64_t)(std::max(0.0, nowSec - heardSec) * 1000.0); }
std::string hex6(uint32_t v) { char b[16]; snprintf(b, sizeof b, "%06X", v & 0xFFFFFF); return b; }
}  // namespace

void observeMesh(const MeshTelemetry& t, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    const int64_t now = wallNowMs();
    TrackBook& nb = book(kModeMesh, rx, 0, Kind::Track, 3600000, 30000);
    for (const MeshNode& n : t.nodes) {
        if (n.id.empty() || n.lastHeard < 0) continue;
        Session s;
        const char* proto = n.protocol == 2 ? "MeshCore" : "Meshtastic";
        s.key = std::string(proto) + " " + n.id;
        s.who[0] = n.id;
        s.alias[0] = n.longName.empty() ? n.shortName : n.longName;
        s.title = std::string(proto) + " " + (s.alias[0].empty() ? n.id : s.alias[0] + " (" + n.id + ")");
        if (n.hasPosition) { s.lat = n.lat; s.lon = n.lon; s.altM = (float)n.altM; }
        s.levelDb = n.lastLevelDb;
        s.snrDb = n.lastSnrDb;
        s.eventCount = n.packets;
        nb.observe(std::move(s), now, seenMs(now, t.timeSec, n.lastHeard));
    }
    TrackBook& db = book(kModeMesh, rx, 1, Kind::Track, 3600000, 30000);
    for (const MeshLorawanDevice& d : t.lorawanDevices) {
        if (d.id.empty() || d.lastHeard < 0) continue;
        Session s;
        s.key = "LoRaWAN " + d.id;
        s.who[0] = d.id;
        s.title = "LoRaWAN " + d.id + (d.vendor.empty() ? "" : " (" + d.vendor + ")");
        s.freqHz = (uint64_t)d.freqHz;
        s.snrDb = d.lastSnrDb;
        s.levelDb = d.lastLevelDb;
        s.eventCount = d.packets;
        db.observe(std::move(s), now, seenMs(now, t.timeSec, d.lastHeard));
    }
    TrackBook& ab = book(kModeMesh, rx, 2, Kind::Track, 1800000, 30000);
    for (const MeshAprsStation& a : t.aprsStations) {
        if (a.st.call.empty()) continue;
        Session s;
        const char* proto = a.protocol == 4 ? "MeshCom" : "LoRa APRS";
        s.key = std::string(proto) + " " + a.st.call;
        s.who[0] = a.st.call;
        s.title = std::string(proto) + " " + a.st.call;
        if (a.st.hasPos) { s.lat = a.st.lat; s.lon = a.st.lon; }
        s.snrDb = a.lastSnrDb;
        s.eventCount = a.st.count;
        ab.observe(std::move(s), now, seenMs(now, t.timeSec, a.st.lastHeardSec));
    }
}

void observeIridium(const IridiumTelemetry& t, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    const int64_t now = wallNowMs();
    TrackBook& sb = book(kModeIridium, rx, 0, Kind::Track, 600000, 15000);
    for (const IridiumSatInfo& i : t.sats) {
        if (i.id < 0 || i.frames == 0) continue;
        Session s;
        s.key = "IRIDIUM SV " + std::to_string(i.id);
        s.who[0] = std::to_string(i.id);
        s.title = "Iridium satellite " + std::to_string(i.id);
        if (i.hasPos) { s.lat = i.lat; s.lon = i.lon; s.altM = (float)(i.altKm * 1000.0); }
        s.eventCount = (uint32_t)std::min<uint64_t>(i.frames, 0xFFFFFFFFu);
        sb.observe(std::move(s), now, seenMs(now, t.timeSec, i.lastHeard));
    }
    TrackBook& ab = book(kModeIridium, rx, 1, Kind::Track, 900000, 15000);
    for (const IridiumAircraft& a : t.aircraft) {
        if (a.reg.empty() && !a.icao) continue;
        Session s;
        const std::string main = !a.reg.empty() ? a.reg : hex6(a.icao);
        s.key = "IRIDIUM ACARS " + main;
        s.who[0] = main;
        s.alias[0] = a.flight;
        s.title = main + (a.flight.empty() ? "" : " (" + a.flight + ")") + " via Iridium";
        s.freqHz = (uint64_t)a.freqHz;
        if (a.hasPos) { s.lat = a.lat; s.lon = a.lon; }
        if (a.hasAlt) s.altM = (float)(a.altFt * 0.3048);
        s.eventCount = a.messages;
        ab.observe(std::move(s), now, seenMs(now, t.timeSec, a.lastHeard));
    }
}

void observeOrbcomm(const OrbcommTelemetry& t, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    const int64_t now = wallNowMs();
    TrackBook& b = book(kModeOrbcomm, rx, 0, Kind::Track, 600000, 15000);
    for (const OrbcommSat& o : t.sats) {
        if (o.id < 0 || (o.frames == 0 && o.packets == 0)) continue;
        Session s;
        s.key = "ORBCOMM FM" + std::to_string(o.id);
        s.who[0] = std::to_string(o.id);
        s.title = "Orbcomm satellite " + std::to_string(o.id);
        s.freqHz = (uint64_t)std::max(0.0, o.freqHz);
        s.snrDb = o.snrDb;
        if (o.hasPos) { s.lat = o.lat; s.lon = o.lon; s.altM = (float)(o.altKm * 1000.0); }
        s.eventCount = (uint32_t)std::min<uint64_t>(o.packets, 0xFFFFFFFFu);
        b.observe(std::move(s), now, seenMs(now, t.timeSec, o.lastHeard));
    }
}

void observeAero(const AeroTelemetry& t, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    const int64_t now = wallNowMs();
    TrackBook& b = book(kModeAero, rx, 0, Kind::Track, 1800000, 30000);
    for (const AeroAircraft& a : t.aircraft) {
        if (a.registration.empty() && !a.aesId) continue;
        Session s;
        const std::string main = !a.registration.empty() ? a.registration : hex6(a.aesId);
        s.key = "AERO " + (a.aesId ? hex6(a.aesId) : main);
        s.who[0] = main;
        s.alias[0] = a.flight;
        s.title = main + (a.flight.empty() ? "" : " (" + a.flight + ")") + " via Inmarsat";
        s.freqHz = (uint64_t)a.heardHz;
        if (a.hasPos) { s.lat = a.lat; s.lon = a.lon; }
        if (a.hasAlt) s.altM = (float)(a.altFt * 0.3048);
        s.eventCount = (uint32_t)std::min<uint64_t>(a.messages, 0xFFFFFFFFu);
        b.observe(std::move(s), now, a.lastWall > 0 ? a.lastWall * 1000 : seenMs(now, t.timeSec, a.lastHeard));
    }
}

void observeInmc(const InmcTelemetry& t, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    const int64_t now = wallNowMs();
    // a safety or news bulletin is one Thread; its repeats are updates of the same one (the text is not stored)
    TrackBook& b = book(kModeInmc, rx, 0, Kind::Thread, 900000, 15000);
    for (const InmcMessage& m : t.messages) {
        if (!m.complete && m.packets == 0) continue;
        Session s;
        char k[48];
        snprintf(k, sizeof k, "EGC %d.%d #%u", m.sat, m.lesId, (unsigned)m.id);
        s.key = k;
        s.who[0] = m.lesName;
        s.title = std::string(m.kind == 1 ? "SafetyNET" : m.kind == 2 ? "FleetNET" : "Inmarsat-C") + " " + (m.serviceText.empty() ? std::string(k) : m.serviceText);
        if (m.priority >= 3) s.flags |= FlagEmergency;
        s.eventCount = m.seen ? m.seen : (uint32_t)m.packets;
        b.observe(std::move(s), now, m.rxUnix > 0 ? m.rxUnix * 1000 : now);
    }
}

void adaptersEndData(uint16_t mode, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    const int64_t now = wallNowMs();
    for (auto& kv : books()) if (std::get<0>(kv.first) == mode && std::get<1>(kv.first) == rx && kv.second) kv.second->endAll(now);
}

void adaptersEndAll() {
    for (uint16_t m : {kModeAdsb, kModeAis, kModeAprs, kModePager}) adaptersEnd(m);
    for (uint16_t m : {(uint16_t)16, (uint16_t)27, (uint16_t)35, (uint16_t)83, (uint16_t)13}) adaptersEndAir(m);
    for (uint16_t m : {kModeMesh, kModeIridium, kModeOrbcomm, kModeAero, kModeInmc}) adaptersEndData(m);
}

}  // namespace dect2::sess
