// APRS / packet path graph (packet_path.h): hops from the H bits, aliases, IGates, loops, silent digipeaters, the relayed-station hull and range.
#include "dect2/packet/packet_path.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static ax25::Address addr(const char* call, int ssid = 0, bool h = false) { ax25::Address a; a.call = call; a.ssid = ssid; a.h = h; return a; }
static ax25::Frame frame(const char* from, std::vector<ax25::Address> path, const char* info = "!hello") {
    ax25::Frame f;
    f.from = addr(from); f.to = addr("APRS"); f.path = std::move(path); f.info = info;
    return f;
}
static const PacketDigi* find(const std::vector<PacketDigi>& v, const std::string& c) { for (auto& d : v) if (d.call == c) return &d; return nullptr; }

int main() {
    PacketPathGraph g;
    std::vector<PacketDigi> dg; std::vector<PacketPathEdge> ed;
    auto snap = [&](double now, std::function<bool(const std::string&, double&, double&)> pos = nullptr) { g.snapshot(now, pos, dg, ed); };

    CHECK(PacketPathGraph::isAlias("WIDE1") && PacketPathGraph::isAlias("WIDE2") && PacketPathGraph::isAlias("TCPIP") && PacketPathGraph::isAlias("RELAY") &&
          !PacketPathGraph::isAlias("WIDEBR") && !PacketPathGraph::isAlias("N0CALL") && !PacketPathGraph::isAlias("NOKIA"), "alias names");

    // direct frame: no hop
    g.observe(frame("N0AAA", {}), 1, false, 0, 0, false);
    snap(2);
    CHECK(dg.empty() && g.directFrames() == 1, "a direct frame makes no digipeater");
    // an alias used as a hop is not a station; a named digipeater is
    g.observe(frame("N0AAA", {addr("WIDE1", 1, true), addr("WIDE2", 1)}), 2, false, 0, 0, false);
    snap(3);
    CHECK(dg.empty(), "WIDE1-1* alone names nobody");
    g.observe(frame("N0AAA", {addr("DIGI1", 0, true), addr("WIDE2", 1)}), 3, true, 51.0, 5.0, false);
    g.observe(frame("N0BBB", {addr("DIGI1", 0, true)}), 4, true, 51.4, 5.3, false);
    g.observe(frame("N0CCC", {addr("DIGI1", 0, true)}), 5, true, 50.7, 4.8, false);
    g.observe(frame("N0DDD", {addr("DIGI1", 0, true)}), 6, true, 51.2, 5.9, false);
    g.observe(frame("N0DDD", {addr("DIGI1", 0, true)}), 7, true, 51.2, 5.9, true);    // a repeat
    snap(8, [](const std::string& c, double& la, double& lo) { if (c == "DIGI1") { la = 51.1; lo = 5.2; return true; } return false; });
    const PacketDigi* d1 = find(dg, "DIGI1");
    CHECK(d1 && d1->heard == 5 && d1->stations == 4 && d1->copies == 1, "DIGI1: heard %u stations %u copies %u", d1 ? d1->heard : 0, d1 ? d1->stations : 0, d1 ? d1->copies : 0);
    CHECK(d1 && d1->hasPos && d1->hull.size() >= 3 && d1->rangeKm > 30 && d1->rangeKm < 70, "hull and range: %zu corners, %.1f km", d1 ? d1->hull.size() : 0, d1 ? d1->rangeKm : 0.0);
    // a chain of two named digipeaters: edges src -> D2 -> D3, the last one is the one we heard
    g.observe(frame("N0EEE", {addr("D2", 0, true), addr("D3", 0, true), addr("WIDE2", 1)}), 10, false, 0, 0, false);
    snap(11);
    bool e1 = false, e2 = false;
    for (auto& e : ed) { if (e.from == "N0EEE" && e.to == "D2") e1 = true; if (e.from == "D2" && e.to == "D3") e2 = true; }
    CHECK(e1 && e2, "hop edges");
    CHECK(find(dg, "D3") && find(dg, "D3")->heard == 1 && find(dg, "D2")->heard == 0 && find(dg, "D2")->hops == 1, "only the last hop was heard directly");
    // an IGate: TCPIP in the path
    g.observe(frame("N0FFF", {addr("TCPIP", 0, false), addr("IGATE1", 0, true)}), 12, false, 0, 0, false);
    // note: TCPIP is not a hop that happened here; the digipeater with the H bit is the gateway that sent it
    snap(13);
    CHECK(find(dg, "IGATE1") && find(dg, "IGATE1")->igate, "IGate recognised");
    CHECK(find(dg, "DIGI1") && !find(dg, "DIGI1")->igate, "a digipeater is not an IGate");
    // loops: a call twice in the path, and the same packet again and again
    PacketPathGraph g2;
    for (int i = 0; i < 6; i++) g2.observe(frame("N0GGG", {addr("LOOPY", 0, true), addr("D9", 0, true), addr("LOOPY", 0, false)}, "!same"), 20 + i, false, 0, 0, true);
    g2.snapshot(40, nullptr, dg, ed);
    CHECK(find(dg, "LOOPY") && find(dg, "LOOPY")->looping, "a path that repeats a call is a loop");
    PacketPathGraph g3;
    for (int i = 0; i < 6; i++) g3.observe(frame("N0HHH", {addr("REPT1", 0, true)}, "!again"), 100 + 10 * i, false, 0, 0, i > 0);
    g3.snapshot(200, nullptr, dg, ed);
    CHECK(find(dg, "REPT1") && find(dg, "REPT1")->looping, "the same packet again and again through one digipeater");
    PacketPathGraph g4;
    for (int i = 0; i < 12; i++) g4.observe(frame("N0III", {addr("GOOD", 0, true)}, ("!n" + std::to_string(i)).c_str()), 100 + 10 * i, false, 0, 0, false);
    g4.snapshot(300, nullptr, dg, ed);
    CHECK(find(dg, "GOOD") && !find(dg, "GOOD")->looping, "different packets are not a loop");
    // silent: heard every 20 s for a while, then nothing for hours
    PacketPathGraph g5;
    for (int i = 0; i < 8; i++) g5.observe(frame("N0JJJ", {addr("OLD", 0, true)}, ("!o" + std::to_string(i)).c_str()), 100 + 20 * i, false, 0, 0, false);
    g5.snapshot(400, nullptr, dg, ed);
    CHECK(find(dg, "OLD") && !find(dg, "OLD")->silent, "recently heard is not silent");
    g5.snapshot(400 + 6 * 3600, nullptr, dg, ed);
    CHECK(find(dg, "OLD") && find(dg, "OLD")->silent, "no frame for hours after a regular beat is silent");
    // shifting the clock moves the times
    g5.shiftTimes(-100);
    g5.snapshot(300, nullptr, dg, ed);
    CHECK(find(dg, "OLD") && find(dg, "OLD")->lastSec < 300 && find(dg, "OLD")->lastSec > 100, "times shift");
    // bounded memory
    PacketPathGraph g6;
    for (int i = 0; i < 2000; i++) { char c[12]; snprintf(c, sizeof c, "D%d", i); g6.observe(frame("N0KKK", {addr(c, 0, true)}, "!x"), i, false, 0, 0, false); }
    g6.snapshot(3000, nullptr, dg, ed);
    CHECK(dg.size() <= 400, "digipeater table bounded: %zu", dg.size());
    return testSummary("packet_path");
}
