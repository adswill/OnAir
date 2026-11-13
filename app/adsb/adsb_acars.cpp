// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "adsb_acars.h"
#include "adsb_internal.h"
#include "state/receivers.h"
#include "dect2/acars/acars_rx.h"
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/vdl2/vdl2_rx.h"
#include "dect2/iridium/iridium_rx.h"
#include <ctime>
#include <map>
#include <set>
#include <unordered_map>

namespace acarslog {

namespace {

dect2::AcarsLog gLog;       // the interesting ones
dect2::AcarsLog gAllLog;    // everything with a readable label (Show all messages)
bool gShowAll = false;
struct Seen { uint64_t seq[4] = {0, 0, 0, 0}, serial[3] = {0, 0, 0}; std::set<uint64_t> done; };
std::map<int, Seen> gSeen;                    // per receiver slot
std::unordered_map<uint32_t, int> gCounts;    // ICAO > number of interesting messages (badge)
std::unordered_map<std::string, int> gHolders;   // normalised callsign > aircraft that have it
double gLastTick = -10;

void put(const dect2::AcarsIdent& id, const char* mode, int64_t wall, double freqHz, const std::string& label, const std::string& text,
         const std::string& flight, bool down) {
    if (id.icao == 0 && id.reg.empty() && id.flight.empty()) return;
    if (label.empty() && text.empty()) return;
    dect2::AcarsLogEntry e;
    e.wall = wall > 0 ? wall : (int64_t)time(nullptr);
    e.mode = mode; e.freqMHz = freqHz / 1e6; e.label = label; e.flight = flight; e.downlink = down;
    e.text = text;
    if (e.text.size() > dect2::kAcarsLogTextMax) e.text.resize(dect2::kAcarsLogTextMax);
    e.cat = dect2::acarsClassify(label, text);
    gLog.add(id, e);
    gAllLog.add(id, e, true);
}

void pollReceiver(App& rx) {
    Seen& s = gSeen[rx.rxId];
    {
        dect2::AcarsTelemetry t;
        if (rx.engine.acars().telemetry(t, s.seq[0])) {
            s.seq[0] = t.seq;
            uint64_t top = s.serial[0];
            for (auto it = t.messages.rbegin(); it != t.messages.rend(); ++it) {     // oldest first
                const auto& m = *it;
                if (m.serial <= s.serial[0] || !m.crcOk) continue;
                top = std::max(top, m.serial);
                dect2::AcarsIdent id; id.reg = m.reg; id.flight = m.flightId;
                put(id, "ACARS", m.wallTime, m.freqHz, m.label, m.text, m.flightId, m.downlink);
            }
            s.serial[0] = top;
        }
    }
    {
        dect2::Vdl2Telemetry t;
        if (rx.engine.vdl2().telemetry(t, s.seq[1])) {
            s.seq[1] = t.seq;
            uint64_t top = s.serial[1];
            for (auto it = t.messages.rbegin(); it != t.messages.rend(); ++it) {
                const auto& m = *it;
                if (m.serial <= s.serial[1]) continue;
                top = std::max(top, m.serial);
                if (!m.acars || !m.acarsCrcOk) continue;
                dect2::AcarsIdent id; id.reg = m.reg; id.flight = m.flight;
                id.icao = m.fromAir ? m.src : m.fromGround ? m.dst : 0;
                put(id, "VDL2", m.wallTime, m.freqHz, m.label, m.acarsText, m.flight, m.fromAir);
            }
            s.serial[1] = top;
        }
    }
    {
        dect2::HfdlTelemetry t;
        if (rx.engine.hfdl().telemetry(t, s.seq[2])) {
            s.seq[2] = t.seq;
            uint64_t top = s.serial[2];
            for (auto it = t.messages.rbegin(); it != t.messages.rend(); ++it) {
                const auto& m = *it;
                if (m.serial <= s.serial[2]) continue;
                top = std::max(top, m.serial);
                if (!m.acars || !m.acarsCrcOk) continue;
                dect2::AcarsIdent id; id.reg = m.reg; id.flight = m.flight; id.icao = m.icao;
                put(id, "HFDL", m.wallTime, m.freqHz, m.label, m.acarsText, m.flight, !m.uplink);
            }
            s.serial[2] = top;
        }
    }
    {   // Iridium ACARS (short burst data): whole messages only; the telemetry list is updated in place when a block joins, so serials are remembered
        dect2::IridiumTelemetry t;
        if (rx.engine.iridium().telemetry(t, s.seq[3])) {
            s.seq[3] = t.seq;
            std::set<uint64_t> alive;
            for (const auto& m : t.acars) {
                alive.insert(m.serial);
                if (m.more || s.done.count(m.serial)) continue;
                s.done.insert(m.serial);
                dect2::AcarsIdent id;
                id.reg = m.regKind == 2 ? std::string() : m.reg; id.flight = m.flight; id.icao = m.icao;
                put(id, "Iridium", m.wall, m.freqHz, m.label, m.text, m.flight, !m.downlink);   // the log's "downlink" is aircraft to ground
            }
            for (auto it = s.done.begin(); it != s.done.end();) it = alive.count(*it) ? std::next(it) : s.done.erase(it);
        }
    }
}

dect2::AcarsAircraftKey keyFor(uint32_t icao, const std::string& callsign) {
    dect2::AcarsAircraftKey k;
    k.icao = icao >> 24 ? 0 : icao;
    k.reg = adsbui::registrationOf(icao);
    k.callsign = callsign;
    auto it = gHolders.find(dect2::acarsNormFlight(callsign));
    k.callsignHolders = it == gHolders.end() ? 1 : it->second;
    return k;
}

}  // namespace

void tick(App& first) {
    (void)first;
    const double now = ImGui::GetTime();
    if (now - gLastTick < 1.0) return;
    gLastTick = now;
    const std::vector<App*> rxs = receiversAll();
    for (App* r : rxs) if (r && r->engine.running()) pollReceiver(*r);
    const int64_t wall = (int64_t)time(nullptr);
    static int pruneN = 0;
    if (++pruneN % 60 == 0) { gLog.prune(wall); gAllLog.prune(wall); }
    gHolders.clear();
    for (App* r : rxs) if (r) for (const auto& ac : r->rx.adsb.aircraft) if (!ac.callsign.empty()) gHolders[dect2::acarsNormFlight(ac.callsign)]++;
    gCounts.clear();
    for (App* r : rxs) if (r) for (const auto& ac : r->rx.adsb.aircraft) {
        if (ac.icao >> 24) continue;
        const size_t n = gLog.count(keyFor(ac.icao, ac.callsign), wall);
        if (n) gCounts[ac.icao] = (int)n;
    }
}

int count(uint32_t icao) {
    auto it = gCounts.find(icao);
    return it == gCounts.end() ? 0 : it->second;
}

std::vector<dect2::AcarsLogEntry> entries(uint32_t icao, const std::string& callsign, bool all) {
    return (all ? gAllLog : gLog).messagesFor(keyFor(icao, callsign), (int64_t)time(nullptr));
}

std::vector<dect2::AcarsLogEntry> forTrack(const std::string& id, const std::string& label, int holders) {
    dect2::AcarsAircraftKey k;
    bool hex = id.size() == 6;
    for (unsigned char c : id) if (!isxdigit(c)) hex = false;
    if (hex) { k.icao = (uint32_t)strtoul(id.c_str(), nullptr, 16); k.reg = adsbui::registrationOf(k.icao); }
    else k.reg = id;
    k.callsign = label;
    k.callsignHolders = holders;
    return gLog.messagesFor(k, (int64_t)time(nullptr));
}

bool& showAll() { return gShowAll; }

std::string timeText(int64_t wall) {
    const std::time_t t = (std::time_t)wall;
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[24];
    std::strftime(b, sizeof b, "%H:%M:%SZ", &tm);
    return b;
}

std::string asText(const std::vector<dect2::AcarsLogEntry>& v) {
    std::string o;
    char b[96];
    for (const auto& e : v) {
        snprintf(b, sizeof b, "%s  %s  %.3f MHz  %s  %s\n", timeText(e.wall).c_str(), e.mode.c_str(), e.freqMHz, e.label.c_str(), dect2::acarsCatName(e.cat));
        o += b;
        o += e.text;
        o += "\n\n";
    }
    return o;
}

} // namespace acarslog
