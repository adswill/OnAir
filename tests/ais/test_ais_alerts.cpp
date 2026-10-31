// AIS filter, CPA / TCPA and geofence alerts (ais_alerts.h), and the receiver raising them from decoded messages.
#include "dect2/ais/ais_alerts.h"
#include "dect2/ais/ais_gen.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/ais/ais_testutil.h"
#include "dect2/automation/catalog.h"
#include <cmath>
#include <cstdio>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static AisVessel ship(uint32_t mmsi, int cls, int type, double lat, double lon, float sog, float cog) {
    AisVessel v; v.mmsi = mmsi; v.cls = cls; v.shipType = type; v.hasPos = true; v.lat = lat; v.lon = lon; v.sog = sog; v.cog = cog; v.name = "S" + std::to_string(mmsi);
    return v;
}

static void filterTests() {
    CHECK(aisShipGroup(-1) == AIS_GRP_UNKNOWN && aisShipGroup(0) == AIS_GRP_UNKNOWN, "unknown group");
    CHECK(aisShipGroup(30) == AIS_GRP_FISHING && aisShipGroup(36) == AIS_GRP_SAILING_PLEASURE && aisShipGroup(52) == AIS_GRP_SERVICE, "groups");
    CHECK(aisShipGroup(69) == AIS_GRP_PASSENGER && aisShipGroup(79) == AIS_GRP_CARGO && aisShipGroup(84) == AIS_GRP_TANKER && aisShipGroup(99) == AIS_GRP_OTHER, "groups 2");
    for (int t = 0; t < 100; t++) CHECK(aisShipGroup(t) >= 0 && aisShipGroup(t) < AIS_GRP_COUNT, "group of %d in range", t);
    AisFilter all;
    CHECK(!all.active(), "default filter is off");
    const AisVessel cargo = ship(1, AIS_CLASS_A, 70, 0, 0, 10, 0), fish = ship(2, AIS_CLASS_A, 30, 0, 0, 3, 0), pleasure = ship(3, AIS_CLASS_B, 37, 0, 0, 0.1f, 0);
    AisVessel base = ship(4, AIS_CLASS_BASE, -1, 0, 0, -1, -1), aton = ship(5, AIS_CLASS_ATON, -1, 0, 0, -1, -1), unk = ship(6, AIS_CLASS_A, -1, 0, 0, 5, 0);
    CHECK(all.matches(cargo) && all.matches(base) && all.matches(aton) && all.matches(unk), "all pass");
    AisFilter f;
    f.classes = 1u << AIS_CLASS_A;
    CHECK(f.active() && f.matches(cargo) && !f.matches(pleasure) && !f.matches(base) && !f.matches(aton), "class A only");
    f = AisFilter();
    f.classes = (1u << AIS_CLASS_BASE) | (1u << AIS_CLASS_ATON);
    f.groups = 1u << AIS_GRP_CARGO;   // ship groups do not apply to fixed stations
    CHECK(f.matches(base) && f.matches(aton) && !f.matches(cargo), "fixed stations ignore the ship-type groups");
    f = AisFilter();
    f.groups = (1u << AIS_GRP_CARGO) | (1u << AIS_GRP_FISHING);
    CHECK(f.matches(cargo) && f.matches(fish) && !f.matches(pleasure) && !f.matches(unk), "cargo and fishing");
    f.groups |= 1u << AIS_GRP_UNKNOWN;
    CHECK(f.matches(unk), "unknown type shown when asked");
    f = AisFilter(); f.movingOnly = true;
    CHECK(f.matches(cargo) && f.matches(fish) && !f.matches(pleasure) && !f.matches(base), "moving only (>= 0.5 kn; no speed = not moving)");
    AisVessel na = ship(7, AIS_CLASS_A, 70, 0, 0, 102.3f, 0);
    CHECK(!f.matches(na), "speed 'not available' is not moving");
    AisFilter g = AisFilter::fromText(f.toText());
    CHECK(g.classes == f.classes && g.groups == f.groups && g.movingOnly && std::fabs(g.minSogKn - f.minSogKn) < 0.01, "text round trip");
    g = AisFilter::fromText("classes=zzz;groups=5;bogus;x=1;moving=1;sog=-3");
    CHECK(g.classes == 0x3F && g.groups == 5 && g.movingOnly && g.minSogKn == 0.5f, "bad fields keep the defaults");
    g = AisFilter::fromText("");
    CHECK(!g.active(), "empty text = no filter");
}

static void cpaTests() {
    // head-on on one meridian: B is 10 nm north and steams south at 10 kn, A stands still: 60 min, passes through A
    AisCpa c = aisCpa(55, 20, 0, 0, 55 + 10.0 / 60, 20, 10, 180);
    CHECK(c.valid && std::fabs(c.distNm - 10) < 0.02 && std::fabs(c.tcpaMin - 60) < 0.5 && c.dcpaNm < 0.05, "head-on: d %.2f tcpa %.1f dcpa %.3f", c.distNm, c.tcpaMin, c.dcpaNm);
    CHECK(std::fabs(c.bearingDeg) < 0.2 || std::fabs(c.bearingDeg - 360) < 0.2, "bearing north %.2f", c.bearingDeg);
    // both moving: A east 10 kn, B 5 nm east on a course of 270 at 10 kn: closing speed 20 kn, 15 min
    c = aisCpa(55, 20, 10, 90, 55, 20 + 5.0 / 60 / std::cos(55 * M_PI / 180), 10, 270);
    CHECK(std::fabs(c.tcpaMin - 15) < 0.4 && c.dcpaNm < 0.05, "both moving: tcpa %.2f dcpa %.3f", c.tcpaMin, c.dcpaNm);
    // crossing: B 5 nm north, going east at 10 kn, A stands: passes 5 nm away at time 0 -> already at closest
    c = aisCpa(0, 0, 0, 0, 5.0 / 60, 0, 10, 90);
    CHECK(std::fabs(c.dcpaNm - 5) < 0.05 && std::fabs(c.tcpaMin) < 0.5, "abeam: dcpa %.2f tcpa %.2f", c.dcpaNm, c.tcpaMin);
    // crossing ahead: B 5 nm north and 5 nm east going south-west... passes 3.5 nm off
    c = aisCpa(0, 0, 0, 0, 5.0 / 60, 5.0 / 60, 10, 225);
    CHECK(c.dcpaNm < 0.1, "south-west bound through the origin: dcpa %.3f", c.dcpaNm);
    c = aisCpa(0, 0, 0, 0, 5.0 / 60, 5.0 / 60, 10, 180);
    CHECK(std::fabs(c.dcpaNm - 5) < 0.05 && c.tcpaMin > 25 && c.tcpaMin < 35, "straight south at 5 nm east: dcpa %.2f tcpa %.1f", c.dcpaNm, c.tcpaMin);
    // diverging: dcpa is the current distance, tcpa negative
    c = aisCpa(55, 20, 0, 0, 55.1, 20, 10, 0);
    CHECK(c.tcpaMin < 0 && std::fabs(c.dcpaNm - c.distNm) < 1e-9, "diverging: tcpa %.1f", c.tcpaMin);
    // same velocity: parallel, no closing
    c = aisCpa(55, 20, 10, 90, 55.05, 20, 10, 90);
    CHECK(c.valid && std::fabs(c.dcpaNm - c.distNm) < 1e-9 && c.tcpaMin == 0, "same velocity");
    // unknown course/speed counts as not moving
    c = aisCpa(55, 20, 0, 0, 55.1, 20, -1, -1);
    CHECK(c.valid && std::fabs(c.dcpaNm - c.distNm) < 1e-9, "unknown motion");
    c = aisCpa(55, 20, 0, 0, 55.1, 20, 102.3f, 0);
    CHECK(c.valid && std::fabs(c.dcpaNm - c.distNm) < 1e-9, "speed not available");
    // across the date line, and near the pole
    c = aisCpa(10, 179.99, 0, 0, 10, -179.99, 10, 90);
    CHECK(c.distNm < 1.3 && c.distNm > 1.1 && c.tcpaMin < 0, "date line: d %.2f tcpa %.1f", c.distNm, c.tcpaMin);
    c = aisCpa(10, -179.99, 0, 0, 10, 179.99, 10, 90);
    CHECK(c.tcpaMin > 0 && c.dcpaNm < 0.05, "date line the other way: tcpa %.2f dcpa %.3f", c.tcpaMin, c.dcpaNm);
    c = aisCpa(89.9, 10, 0, 0, 89.9, 190, 5, 0);
    CHECK(std::isfinite(c.dcpaNm) && std::isfinite(c.tcpaMin), "pole is finite");
    c = aisCpa(NAN, 0, 0, 0, 1, 1, 1, 1);
    CHECK(!c.valid, "NaN input is not valid");
    CHECK(std::fabs(aisDistanceNm(0, 0, 1, 0) - 60.04) < 0.05, "one degree = 60 nm: %.3f", aisDistanceNm(0, 0, 1, 0));
}

static void fenceTests() {
    std::vector<std::string> err;
    const std::string txt = "# my fences\nHarbour;54.7,25.3,2\nBay;poly;54.0,20.0 54.0,21.0 55.0,21.0 55.0,20.0\n\nbad1;1,2\nbad2;poly;1,2 3,4\nbad3;95,0,1\n;1,1,1\nbad4;1,1,-3\nbad5;poly;1,2 x,y 5,6\n";
    const auto f = aisParseGeofences(txt, &err);
    CHECK(f.size() == 2 && err.size() == 6, "parsed %zu fences, %zu errors", f.size(), err.size());
    if (f.size() == 2) {
        CHECK(f[0].name == "Harbour" && !f[0].polygon && f[0].contains(54.7, 25.3) && f[0].contains(54.7, 25.3 + 1.9 / 60 / std::cos(54.7 * M_PI / 180)) &&
                  !f[0].contains(54.7, 25.3 + 2.2 / 60 / std::cos(54.7 * M_PI / 180)),
              "circle");
        CHECK(f[1].polygon && f[1].contains(54.5, 20.5) && !f[1].contains(55.5, 20.5) && !f[1].contains(54.5, 21.5) && !f[1].contains(53.9, 20.5), "polygon");
        const auto g = aisParseGeofences(aisGeofencesToText(f));
        CHECK(g.size() == 2 && g[1].pts.size() == 4 && g[1].contains(54.5, 20.5) && g[0].contains(54.7, 25.3), "text round trip");
    }
    // a polygon over the date line
    const auto d = aisParseGeofences("dl;poly;10,179 10,-179 11,-179 11,179\n");
    CHECK(d.size() == 1 && d[0].contains(10.5, 179.5) && d[0].contains(10.5, -179.5) && !d[0].contains(10.5, 170) && !d[0].contains(10.5, -170), "date line polygon");
    // concave polygon (an L): the notch is outside
    const auto l = aisParseGeofences("L;poly;0,0 0,2 1,2 1,1 2,1 2,0\n");
    CHECK(l.size() == 1 && l[0].contains(0.5, 1.5) && l[0].contains(1.5, 0.5) && !l[0].contains(1.5, 1.5), "concave polygon");
    CHECK(aisParseGeofences("", nullptr).empty() && aisParseGeofences("\r\n\r\n").empty(), "empty text");
    std::string many;
    for (int i = 0; i < 100; i++) many += "f" + std::to_string(i) + ";1,1,1\n";
    CHECK(aisParseGeofences(many).size() == 32, "at most 32 fences");
}

static void engineTests() {
    AisAlertEngine e;
    AisAlertConfig c;
    // off by default
    CHECK(e.update(ship(1, AIS_CLASS_A, 70, 55, 20, 10, 0), 1000).empty(), "nothing configured");
    c.cpaOn = true; c.ownValid = true; c.ownLat = 55; c.ownLon = 20; c.cpaNm = 0.5; c.tcpaMin = 15; c.cooldownSec = 600;
    e.configure(c);
    // 3 nm north, steaming south at 12 kn: tcpa 15 min -> alert
    AisVessel v = ship(100, AIS_CLASS_A, 70, 55 + 3.0 / 60, 20, 12, 180);
    auto a = e.update(v, 1000);
    CHECK(a.size() == 1 && a[0].kind == "cpa" && a[0].dcpaNm < 0.1 && std::fabs(a[0].tcpaMin - 15) < 0.5 && std::fabs(a[0].distNm - 3) < 0.05, "cpa alert (%zu)", a.size());
    CHECK(e.update(v, 1010).empty(), "no repeat while active");
    v.lat = 55 + 1.0 / 60;
    CHECK(e.update(v, 1100).empty(), "no repeat after 100 s (cooldown)");
    // passes and goes away, comes back after the cooldown
    v.lat = 55 - 1.0 / 60; v.cog = 180;
    CHECK(e.update(v, 1200).empty(), "diverging");
    v.lat = 55 + 3.0 / 60; v.cog = 180;
    CHECK(e.update(v, 1500).empty(), "back inside the cooldown");
    CHECK(e.update(v, 1700).empty(), "still on the course: quiet");
    v.lat = 55 - 1.0 / 60;
    CHECK(e.update(v, 1750).empty(), "gone");
    v.lat = 55 + 3.0 / 60;
    CHECK(e.update(v, 1800).size() == 1, "alert again after leaving the danger and the cooldown");
    // not on a collision course
    CHECK(e.update(ship(101, AIS_CLASS_A, 70, 55 + 3.0 / 60, 20 + 0.2, 12, 180), 2000).empty(), "passing 7 nm off: no alert");
    // too far in time
    CHECK(e.update(ship(102, AIS_CLASS_A, 70, 55 + 10.0 / 60, 20, 12, 180), 2000).empty(), "50 min away: no alert");
    // unknown course but standing right next to the receiver
    CHECK(e.update(ship(103, AIS_CLASS_A, 70, 55 + 0.2 / 60, 20, -1, -1), 2000).size() == 1, "very close, no motion known: alert");
    CHECK(e.update(ship(104, AIS_CLASS_A, 70, 55 + 4.0 / 60, 20, -1, -1), 2000).empty(), "far, no motion known: no alert");
    // base stations and aids never raise a CPA
    CHECK(e.update(ship(105, AIS_CLASS_BASE, -1, 55 + 0.1 / 60, 20, 0, 0), 2000).empty() && e.update(ship(106, AIS_CLASS_ATON, -1, 55 + 0.1 / 60, 20, 0, 0), 2000).empty(), "fixed stations");
    // stale positions are ignored
    AisVessel old = ship(107, AIS_CLASS_A, 70, 55 + 3.0 / 60, 20, 12, 180); old.lastUtc = 1000;
    CHECK(e.update(old, 1500).empty(), "old position ignored");
    // the filter limits who raises alerts
    c.filter.groups = 1u << AIS_GRP_TANKER;
    e.configure(c);
    CHECK(e.update(ship(108, AIS_CLASS_A, 70, 55 + 3.0 / 60, 20, 12, 180), 3000).empty(), "cargo filtered out");
    CHECK(e.update(ship(109, AIS_CLASS_A, 80, 55 + 3.0 / 60, 20, 12, 180), 3000).size() == 1, "tanker passes the filter");
    // moving receiver: own ship heading north at 10 kn, other ship 5 nm ahead heading south at 10 kn: 15 min
    AisAlertConfig m = c; m.filter = AisFilter(); m.ownSogKn = 10; m.ownCogDeg = 0;
    e.configure(m);
    a = e.update(ship(110, AIS_CLASS_A, 70, 55 + 4.8 / 60, 20, 10, 180), 4000);
    CHECK(a.size() == 1 && std::fabs(a[0].tcpaMin - 14.4) < 0.5, "own ship moving: tcpa %.1f", a.empty() ? -1 : a[0].tcpaMin);
    // CPA off: nothing
    m.cpaOn = false;
    e.configure(m);
    CHECK(e.update(ship(111, AIS_CLASS_A, 70, 55 + 1.0 / 60, 20, 12, 180), 5000).empty(), "cpa off");

    // geofence: enter / exit, first sighting inside raises nothing, two fences
    AisAlertEngine g;
    AisAlertConfig fc;
    fc.fences = aisParseGeofences("A;55,20,1\nB;55,20,3\n");
    g.configure(fc);
    AisVessel s = ship(200, AIS_CLASS_A, 70, 55 + 5.0 / 60, 20, 10, 180);
    CHECK(g.update(s, 100).empty(), "first sighting outside");
    s.lat = 55 + 2.0 / 60;
    a = g.update(s, 110);
    CHECK(a.size() == 1 && a[0].kind == "enter" && a[0].fence == "B", "enters B");
    s.lat = 55 + 0.5 / 60;
    a = g.update(s, 120);
    CHECK(a.size() == 1 && a[0].kind == "enter" && a[0].fence == "A", "enters A");
    CHECK(g.update(s, 125).empty(), "inside: no repeat");
    s.lat = 55 - 4.0 / 60;
    a = g.update(s, 130);
    CHECK(a.size() == 2 && a[0].kind == "exit" && a[1].kind == "exit", "leaves both");
    AisVessel t = ship(201, AIS_CLASS_A, 70, 55, 20, 0, 0);
    CHECK(g.update(t, 140).empty(), "first sighting inside does not enter");
    t.lat = 55 + 10.0 / 60;
    a = g.update(t, 150);
    CHECK(a.size() == 2, "then leaves: %zu", a.size());
    // reconfigure keeps what ships are inside
    AisVessel u = ship(202, AIS_CLASS_A, 70, 55, 20, 0, 0);
    g.update(u, 160);
    g.configure(fc);
    CHECK(g.update(u, 170).empty(), "reconfigure does not re-enter");
    // a vessel without a position raises nothing
    AisVessel np; np.mmsi = 300;
    CHECK(g.update(np, 1).empty(), "no position");
}

// ---- the receiver: decoded messages raise alerts
static ais::Bits posReport(uint32_t mmsi, double lat, double lon, double sogKn, double cogDeg) {
    using namespace ais;
    Bits b;
    putU(b, 0, 6, 1);
    putU(b, 8, 30, mmsi);
    putU(b, 38, 4, 0);
    putS(b, 42, 8, -128);
    putU(b, 50, 10, (uint32_t)std::lround(sogKn * 10));
    putU(b, 60, 1, 1);
    putS(b, 61, 28, (int32_t)std::lround(lon * 600000));
    putS(b, 89, 27, (int32_t)std::lround(lat * 600000));
    putU(b, 116, 12, (uint32_t)std::lround(cogDeg * 10));
    putU(b, 128, 9, 511);
    putU(b, 137, 6, 60);
    b.resize(168, 0);
    return b;
}

static void receiverTests() {
    const double lat0 = 54.69, lon0 = 25.28;
    const double kPerMin = 1.0 / 60 / std::cos(lat0 * M_PI / 180);   // degrees of longitude per nautical mile
    std::vector<AisBurstSpec> sp;
    // ship 1 comes from the east heading west at 10 kn: 4.3 nm out then at the fence centre
    AisBurstSpec a; a.channel = 'A'; a.startSec = 0.1; a.payload = posReport(219000111, lat0, lon0 + 4.3 * kPerMin, 10, 270); sp.push_back(a);
    AisBurstSpec b = a; b.startSec = 0.6; b.payload = posReport(219000111, lat0, lon0, 10, 270); sp.push_back(b);
    // ship 2 is a fishing boat, outside every limit
    AisBurstSpec c = a; c.channel = 'B'; c.startSec = 0.3; c.payload = posReport(219000222, lat0 + 0.3, lon0, 4, 0); sp.push_back(c);
    AisRenderConfig rc; rc.rate = 1e6; rc.durationSec = 1.0; rc.snrDb = 25;
    const std::vector<cf32> x = renderAisBursts(sp, rc);
    AisReceiver rx; rx.configure(rc.rate);
    AisAlertConfig ac;
    ac.cpaOn = true; ac.ownValid = true; ac.ownLat = lat0; ac.ownLon = lon0; ac.cpaNm = 0.5; ac.tcpaMin = 30;
    ac.fences = aisParseGeofences("Quay;54.69,25.28,1\n");
    rx.setAlertConfig(ac);
    const AisTelemetry t = aistest::runReceiver(rx, x);
    CHECK(t.blocksOk >= 3, "decoded %llu", (unsigned long long)t.blocksOk);
    int cpa = 0, enter = 0, exitN = 0, other = 0;
    for (const auto& r : t.alerts) {
        if (r.mmsi != 219000111) other++;
        else if (r.kind == "cpa") cpa++;
        else if (r.kind == "enter" && r.fence == "Quay") enter++;
        else if (r.kind == "exit") exitN++;
    }
    CHECK(cpa == 1 && enter == 1 && exitN == 0 && other == 0, "alerts: cpa %d enter %d exit %d other %d (of %zu)", cpa, enter, exitN, other, t.alerts.size());
    // without a config nothing is raised; clearResults empties the list
    AisReceiver r2; r2.configure(rc.rate);
    CHECK(aistest::runReceiver(r2, x).alerts.empty(), "no alerts when off");
    rx.clearResults();
    AisTelemetry t2; rx.telemetry(t2, 0);
    CHECK(t2.alerts.empty(), "clearResults empties the alerts");
}

static void catalogTests() {
    using namespace automation;
    const EventInfo* c = findEventInfo("ais.cpa");
    const EventInfo* g = findEventInfo("ais.geofence");
    CHECK(c && g, "events in the catalog");
    if (c && g) {
        const Event e = sampleEvent("ais.cpa", 1);
        CHECK(e.f.count("cpa_nm") && e.f.count("tcpa_min") && e.f.count("mmsi"), "cpa sample");
        CHECK(sampleEvent("ais.geofence", 1).f.count("fence") && triggerMatches("ais.*", "ais.cpa") && triggerMatches("ais.*", "ais.geofence"), "geofence sample, ais.* trigger");
    }
}

int main() {
    filterTests();
    cpaTests();
    fenceTests();
    engineTests();
    receiverTests();
    catalogTests();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ais alerts OK\n");
    return 0;
}
