#include "dect2/session/mode_adapters.h"
#include "dect2/adsb/adsb_tel.h"
#include "dect2/ais/ais_tel.h"
#include "dect2/packet/packet_tel.h"
#include "dect2/pager/pager_tel.h"
#include "dect2/session/track_book.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>

namespace dect2::sess {
namespace {

struct Ctx {
    std::unique_ptr<TrackBook> book;
    uint64_t lastSerial = 0;          // pager: highest message serial taken
    double lastFrameSec = -1e9;       // packet: signal time of the last frame taken
    std::map<std::string, uint32_t> counts;
};
std::mutex gMu;
std::map<std::pair<uint16_t, uint16_t>, Ctx>& ctxs() { static auto* m = new std::map<std::pair<uint16_t, uint16_t>, Ctx>; return *m; }

Ctx& ctx(uint16_t mode, uint16_t rx, Kind kind, int64_t gapMs, int64_t updateMs) {
    Ctx& c = ctxs()[{mode, rx}];
    if (!c.book) {
        BookConfig bc; bc.mode = mode; bc.receiver = rx; bc.kind = kind; bc.gapMs = gapMs; bc.updateMs = updateMs;
        c.book.reset(new TrackBook(bc));
    }
    return c;
}

std::string hex6(uint32_t v) { char b[16]; snprintf(b, sizeof b, "%06X", v & 0xFFFFFF); return b; }
}  // namespace

void observeAdsb(const AdsbTelemetry& t, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    Ctx& c = ctx(kModeAdsb, rx, Kind::Track, 120000, 5000);
    const int64_t now = wallNowMs();
    for (const AdsbAircraft& a : t.aircraft) {
        Session s;
        const std::string icao = hex6(a.icao);
        s.key = "ICAO " + icao;
        s.who[0] = icao;
        s.alias[0] = a.callsign;
        s.title = a.callsign.empty() ? "Aircraft " + icao : a.callsign + " (" + icao + ")";
        if (a.hasPos) { s.lat = a.lat; s.lon = a.lon; }
        if (a.hasAlt) s.altM = (float)(a.altFt * 0.3048);
        s.levelDb = a.levelDbfs;
        s.eventCount = a.messages;
        if (a.emergency) s.flags |= FlagEmergency;
        s.freqHz = 1090000000ull;
        c.book->observe(std::move(s), now, now - (int64_t)(a.ageSec * 1000.f));
    }
}

void observeAis(const AisTelemetry& t, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    Ctx& c = ctx(kModeAis, rx, Kind::Track, 600000, 10000);
    const int64_t now = wallNowMs();
    for (const AisVessel& v : t.vessels) {
        Session s;
        const std::string id = std::to_string(v.mmsi);
        s.key = "MMSI " + id;
        s.who[0] = id;
        s.alias[0] = v.name.empty() ? v.callsign : v.name;
        s.title = v.name.empty() ? "Vessel " + id : v.name + " (" + id + ")";
        if (v.hasPos) { s.lat = v.lat; s.lon = v.lon; }
        s.eventCount = v.messages;
        s.freqHz = (v.channel == 'B') ? 162025000ull : 161975000ull;
        c.book->observe(std::move(s), now, now - (int64_t)(v.ageSec * 1000.f));
    }
}

void observePacket(const PacketTelemetry& t, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    Ctx& c = ctx(kModeAprs, rx, Kind::Track, 600000, 10000);
    Ctx& th = ctx(kModeAprs, (uint16_t)(rx + 1000), Kind::Thread, 600000, 5000);   // the message threads live in their own book
    const int64_t now = wallNowMs();
    if (t.timeSec < c.lastFrameSec) c.lastFrameSec = -1e9;   // the receiver restarted: its clock began again
    for (const PacketStation& st : t.stations) {
        if (st.isObject && st.call.empty()) continue;
        Session s;
        s.key = "station " + st.call;
        s.who[0] = st.call;
        s.title = (st.isObject ? "Object " : "Station ") + st.call;
        if (st.hasPos) { s.lat = st.lat; s.lon = st.lon; }
        if (st.hasAlt) s.altM = (float)st.altM;
        s.eventCount = st.count;
        s.carrierId = st.carrierId;
        c.book->observe(std::move(s), now, now - (int64_t)((t.timeSec - st.lastHeardSec) * 1000.0));
    }
    for (const PacketFrameInfo& f : t.frames) {
        if (f.timeSec <= c.lastFrameSec) continue;
        c.lastFrameSec = std::max(c.lastFrameSec, f.timeSec);
        if (f.type != "Message") continue;
        const std::string a = std::min(f.from, f.to), b = std::max(f.from, f.to);
        Session s;
        s.key = a + " <> " + b;
        s.who[0] = f.from; s.who[1] = f.to;
        uint32_t& n = th.counts[s.key];
        n++;
        s.eventCount = n;
        s.title = "APRS messages " + a + " <> " + b;
        if (n > 1) s.title += ", " + std::to_string(n);
        th.book->observe(std::move(s), now, now - (int64_t)((t.timeSec - f.timeSec) * 1000.0));
    }
}

void observePager(const PagerTelemetry& t, uint16_t rx) {
    if (!t.messages) return;
    std::lock_guard<std::mutex> lk(gMu);
    Ctx& c = ctx(kModePager, rx, Kind::Thread, 600000, 3000);
    const int64_t now = wallNowMs();
    uint64_t hi = c.lastSerial;
    const auto& v = *t.messages;                    // newest first: take the new ones oldest first
    size_t n = 0;
    while (n < v.size() && v[n].serial > c.lastSerial) n++;
    for (size_t i = n; i-- > 0;) {
        const PagerMessage& m = v[i];
        hi = std::max(hi, m.serial);
        Session s;
        const std::string cap = std::to_string(m.address);
        s.key = std::string(m.flex ? "FLEX " : "POCSAG ") + cap;
        s.who[0] = cap;
        uint32_t& cnt = c.counts[s.key];
        cnt++;
        s.eventCount = cnt;
        s.title = std::string(m.flex ? "FLEX " : "POCSAG ") + cap + ", " + std::to_string(cnt) + (cnt == 1 ? " page" : " pages");
        s.carrierId = m.carrierId;
        if (m.damaged) s.verdict = (uint8_t)Verdict::Unreadable;
        if (m.type == kPagerSecure) s.verdict = (uint8_t)Verdict::Encrypted;
        const int64_t seen = m.wallTime > 0 ? m.wallTime * 1000 : now;
        c.book->observe(std::move(s), std::max(now, seen), seen);
    }
    c.lastSerial = hi;
}

void adaptersEnd(uint16_t mode, uint16_t rx) {
    std::lock_guard<std::mutex> lk(gMu);
    const int64_t now = wallNowMs();
    for (uint16_t r : {rx, (uint16_t)(rx + 1000)}) {
        auto it = ctxs().find({mode, r});
        if (it == ctxs().end()) continue;
        if (it->second.book) it->second.book->endAll(now);
        it->second.counts.clear();
        it->second.lastSerial = 0;
        it->second.lastFrameSec = -1e9;
    }
}

}  // namespace dect2::sess
