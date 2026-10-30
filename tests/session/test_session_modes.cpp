// The mode adapters (ADS-B, AIS, APRS, POCSAG/FLEX -> Sessions): first sight begins, later sights update, silence ends; pager ids are hashed;
// nothing from a message body is stored; a stale key is not a new session.
#include "dect2/adsb/adsb_tel.h"
#include "dect2/ais/ais_tel.h"
#include "dect2/packet/packet_tel.h"
#include "dect2/pager/pager_tel.h"
#include "dect2/session/mode_adapters.h"
#include "dect2/session/session_store.h"
#include "dect2/session/track_book.h"
#include "session_test_util.h"
#include <cstdio>
using namespace dect2;
using namespace dect2::sess;
#include "../common/test_util.h"

static const Session* find(const std::vector<Session>& v, const std::string& sub) {
    for (const Session& s : v) if (s.key.find(sub) != std::string::npos || s.title.find(sub) != std::string::npos) return &s;
    return nullptr;
}

static void testBook() {
    sesstest::TempDir td;
    SessionStore st;
    CHECK(st.openAt(td.file("b.db")), "open");
    BookConfig bc; bc.mode = 11; bc.kind = Kind::Track; bc.gapMs = 1000; bc.updateMs = 100;
    TrackBook b(bc, &st);
    Session s; s.key = "ICAO 4CA7B1"; s.title = "RYR1"; s.eventCount = 1;
    const int64_t t0 = sesstest::nowMs();
    uint64_t id = b.observe(s, t0);
    CHECK(id != 0 && b.openCount() == 1, "first sight begins");
    s.eventCount = 5;
    CHECK(b.observe(s, t0 + 500) == id, "same key, same session");
    Session stale; stale.key = "ICAO 000001";
    CHECK(b.observe(stale, t0, t0 - 5000) == 0, "an already silent key is not a new session");
    b.sweep(t0 + 3000);
    st.flush();
    CHECK(b.openCount() == 0, "ended after the gap");
    Session got;
    CHECK(st.get(id, got) && got.endWall != 0 && got.eventCount == 5, "the stored session is ended with the last update");
    st.close();
}

static void testAdapters() {
    sesstest::TempDir td;
    SessionStore& st = global();
    CHECK(st.openAt(td.file("g.db")), "global store opens");
    AdsbTelemetry a; AdsbAircraft ac; ac.icao = 0x4CA7B1; ac.callsign = "RYR1AB"; ac.hasPos = true; ac.lat = 54.6; ac.lon = 25.2; ac.hasAlt = true; ac.altFt = 30000; ac.messages = 12; ac.ageSec = 1;
    a.aircraft.push_back(ac);
    observeAdsb(a);
    AisTelemetry s; AisVessel v; v.mmsi = 276123456; v.name = "BALTIC STAR"; v.hasPos = true; v.lat = 59.4; v.lon = 24.7; v.messages = 3; v.ageSec = 2;
    s.vessels.push_back(v);
    observeAis(s);
    PacketTelemetry p; p.timeSec = 100; PacketStation ps; ps.call = "LY1AB-9"; ps.hasPos = true; ps.lat = 54.7; ps.lon = 25.3; ps.count = 4; ps.lastHeardSec = 99; p.stations.push_back(ps);
    PacketFrameInfo f; f.timeSec = 99; f.from = "LY1AB-9"; f.to = "LY2CD"; f.type = "Message"; f.info = ":LY2CD    :secret body{1"; p.frames.push_back(f);
    observePacket(p);
    PagerTelemetry g; auto msgs = std::make_shared<std::vector<PagerMessage>>();
    PagerMessage m; m.serial = 1; m.address = 1234567; m.flex = false; m.text = "SECRET PAGE TEXT"; m.wallTime = sesstest::nowMs() / 1000; msgs->push_back(m);
    g.messages = msgs;
    observePager(g);
    observePager(g);   // the same message again is not counted twice
    st.flush();
    auto w = st.window(kModeAdsb);
    CHECK(w.size() == 1 && w[0].kind == (uint8_t)Kind::Track && w[0].title.find("RYR1AB") != std::string::npos && w[0].hasPosition(), "ADS-B track with callsign and position");
    w = st.window(kModeAis);
    CHECK(w.size() == 1 && w[0].title.find("BALTIC STAR") != std::string::npos, "AIS track with the vessel name");
    w = st.window(kModeAprs);
    CHECK(find(w, "station LY1AB-9") && find(w, "<>"), "APRS station track and message thread");
    const Session* th = find(w, "<>");
    CHECK(th && th->kind == (uint8_t)Kind::Thread && th->title.find("secret") == std::string::npos, "the thread holds no message body");
    w = st.window(kModePager);
    CHECK(w.size() == 1 && w[0].kind == (uint8_t)Kind::Thread && w[0].eventCount == 1, "one pager thread, one page");
    CHECK(w.size() == 1 && w[0].key.find("1234567") == std::string::npos && w[0].title.find("1234567") == std::string::npos, "the capcode is hashed, not stored");
    CHECK(w.size() == 1 && w[0].title.find("SECRET") == std::string::npos, "no page text");
    adaptersEnd(kModeAdsb);
    st.flush();
    w = st.window(kModeAdsb);
    CHECK(w.size() == 1 && w[0].endWall != 0, "a mode change ends the open tracks");
    st.close();
}

int main() {
    testBook();
    testAdapters();
    return testSummary("session_modes");
}
