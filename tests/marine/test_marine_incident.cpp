// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The incident board (marine_incident.h): DSC distress + AIS last position + AIS-SART / MOB / EPIRB + vessels nearby + NAVTEX, and the NAVTEX geocoder.
#include "dect2/marine/marine_dsc.h"
#include "dect2/marine/marine_incident.h"
#include "dect2/marine/marine_navtex_geo.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::marine;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static AisVessel ship(uint32_t mmsi, double lat, double lon, float sog = 0, float cog = 0, const char* name = "", int cls = AIS_CLASS_A, int nav = 0) {
    AisVessel v; v.mmsi = mmsi; v.hasPos = true; v.lat = lat; v.lon = lon; v.sog = sog; v.cog = cog; v.name = name; v.cls = cls; v.navStatus = nav;
    return v;
}
static DscCall distress(const char* from, bool pos, double lat, double lon, const char* kind = "Distress alert") {
    DscCall c; c.format = 112; c.formatName = "Distress alert"; c.category = 112; c.distress = true; c.fromMmsi = from; c.nature = 101; c.natureName = "Sinking";
    c.hasPos = pos; c.lat = lat; c.lon = lon; c.hasTime = true; c.utcHour = 12; c.utcMin = 34; c.eccOk = true; c.kind = kind; c.fromCountry = "Germany";
    c.text = "Distress alert";
    return c;
}
static NavtexMessage nav(const char* header, char subj, const char* subjName, const char* text) {
    NavtexMessage m; m.header = header; m.station = header[0]; m.subject = subj; m.subjectName = subjName; m.text = text; m.complete = true;
    return m;
}
static bool near(double a, double b, double e) { return std::fabs(a - b) <= e; }

static void geoTests() {
    struct T { const char* s; double la, lo; };
    const T t[] = {
        {"HIT AND RUN AT 56-30N 003-15E.", 56.5, 3.25},
        {"POSITION 56-30.5N 003-15.2E", 56.508333, 3.2533},
        {"POSITION 5630N 00315E MASTER", 56.5, 3.25},
        {"IN 5630.5N 00315.2E", 56.508333, 3.2533},
        {"LAT 56 30 N LONG 003 15 W", 56.5, -3.25},
        {"N56 30 E003 15", 56.5, 3.25},
        {"N 56-30 E 003-15", 56.5, 3.25},
        {"56.5N 3.25E", 56.5, 3.25},
        {"56.5N, 3.25W", 56.5, -3.25},
        {"34-10S 018-25E", -34.1666667, 18.416667},
        {"56\xC2\xB0" "30'N 003\xC2\xB0" "15'E", 56.5, 3.25},
        {"56-30n 003-15e", 56.5, 3.25},
        {"5530N 02115E.", 55.5, 21.25},
    };
    for (const T& x : t) {
        const auto g = navtexPositions(x.s);
        CHECK(g.size() == 1 && near(g[0].lat, x.la, 0.001) && near(g[0].lon, x.lo, 0.001), "geo '%s' -> %zu hits (%.4f %.4f)", x.s, g.size(), g.empty() ? 0 : g[0].lat, g.empty() ? 0 : g[0].lon);
    }
    // refused: minutes over 59, latitude over 90, longitude over 180, plain numbers
    CHECK(navtexPositions("56-75N 003-15E").empty(), "minutes 75");
    CHECK(navtexPositions("95-10N 003-15E").empty(), "latitude 95");
    CHECK(navtexPositions("56-10N 193-15E").empty(), "longitude 193");
    CHECK(navtexPositions("WIND 25 KT GUSTS 40 KT VISIBILITY 5 NM SWELL 2 M").empty(), "no positions in weather text");
    CHECK(navtexPositions("101200 UTC NOV 25 NR 12").empty(), "no positions in dates");
    // two positions in order, and a line of text between
    const auto g2 = navtexPositions("BUOY MOVED FROM 55-00N 010-00E TO 55-05N 010-10E");
    CHECK(g2.size() == 2 && near(g2[1].lat, 55.0833, 0.001) && near(g2[1].lon, 10.1667, 0.001), "two positions");
    // damaged characters '*' and mixed case
    CHECK(navtexPositions("pos 56-3*N 003-15E").empty(), "damaged position not guessed");
    const auto a = navtexAreas("Gale warning for the North Sea and Skagerrak, and the Gulf of Finland");
    CHECK(a.size() == 3, "areas %zu", a.size());
    CHECK(navtexAreas("NORTHSEA ROUTE").empty() && navtexAreas("SOUNDING").empty(), "no partial words");
    CHECK(navtexAreas("BALTIC").size() == 1 && navtexAreas("BAY OF BISCAY").size() == 1, "alias");
}

static void dscScenario() {
    IncidentBoard b;
    b.setOwn(true, 54.0, 12.0);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> r(-0.4, 0.4);
    const double sLat = 54.8, sLon = 13.2;
    int nIn = 0;
    for (int k = 0; k < 120; k++) {   // traffic around the scene
        const double la = sLat + r(rng), lo = sLon + r(rng) * 2;
        b.onAis(ship(211000000 + k, la, lo, 8 + k % 7, (float)(k * 13 % 360), "TRAFFIC"), 1000);
        if (incidentDistanceNm(sLat, sLon, la, lo) <= 20) nIn++;
    }
    b.onAis(ship(244123456, sLat + 0.01, sLon + 0.01, 0, 0, "ZEEVALK"), 1000);
    CHECK(b.openCount() == 0, "AIS alone opens nothing");
    CHECK(b.takeAlerts().empty(), "no alert");
    DscCall c = distress("244123456", true, sLat, sLon);
    c.distressMmsi = "";
    b.onDsc(c, 1100);
    auto s = b.snapshot();
    CHECK(s.size() == 1 && s[0].kind == "DSC distress" && s[0].pinned && s[0].mmsi == "244123456", "incident opened");
    if (s.size() == 1) {
        const Incident& i = s[0];
        CHECK(i.hasDscPos && i.hasAisPos && i.posSource == "DSC" && i.posGapNm > 0 && i.posGapNm < 1.5 && !i.posGapFlag, "positions gap %.2f", i.posGapNm);
        CHECK(i.name == "ZEEVALK", "name from AIS");
        CHECK(i.hasOwn && near(i.distNm, incidentDistanceNm(54, 12, sLat, sLon), 0.1) && i.bearingDeg > 40 && i.bearingDeg < 70, "own distance %.1f bearing %.0f", i.distNm, i.bearingDeg);
        CHECK(!i.nearby.empty() && i.nearby.size() <= 12, "nearby %zu", i.nearby.size());
        for (size_t k = 1; k < i.nearby.size(); k++) CHECK(i.nearby[k - 1].distNm <= i.nearby[k].distNm, "nearby sorted");
        for (const auto& n : i.nearby) CHECK(n.distNm <= 20.0 + 1e-6 && n.mmsi != 244123456, "nearby inside 20 nm");
        CHECK(i.nature == "Sinking" && i.dscCalls == 1, "nature");
    }
    auto al = b.takeAlerts();
    CHECK(al.size() == 1 && al[0].what == "opened", "one pinned alert (%zu)", al.size());
    // the call is repeated (5 s apart, then each minute): merged, no new alert, no new incident
    for (int k = 0; k < 10; k++) b.onDsc(c, 1105 + k * 60);
    s = b.snapshot();
    CHECK(s.size() == 1 && s[0].dscCalls == 11, "repeats merged: %d", s.empty() ? -1 : s[0].dscCalls);
    CHECK(b.takeAlerts().empty(), "repeats raise nothing");
    size_t dscEv = 0;
    for (auto& e : s[0].timeline) if (e.source == "DSC") dscEv++;
    CHECK(dscEv <= 3, "timeline keeps repeats as one line, got %zu DSC lines", dscEv);
    // acknowledgement by a coast station
    DscCall ack = distress("002111240", false, 0, 0, "Distress alert acknowledgement");
    ack.distressMmsi = "244123456"; ack.fromMmsi = "002111240";
    b.onDsc(ack, 1300);
    s = b.snapshot();
    CHECK(s.size() == 1 && s[0].acked && s[0].ackedBy == "002111240", "ack");
    // a call that failed its check, or with unreadable digits, does nothing
    DscCall bad = distress("244123457", true, 54, 12); bad.eccOk = false;
    b.onDsc(bad, 1400);
    DscCall bad2 = distress("2441~3458", true, 54, 12);
    b.onDsc(bad2, 1400);
    CHECK(b.snapshot().size() == 1, "damaged calls ignored");
    (void)nIn;
}

static void gapTests() {
    IncidentBoard b;
    b.onAis(ship(244000111, 54.0, 12.0, 0, 0, "A"), 100);
    b.onDsc(distress("244000111", true, 54.7, 12.0), 200);   // 42 nm north of the last AIS position
    auto s = b.snapshot();
    CHECK(s.size() == 1 && s[0].posGapFlag && s[0].posGapNm > 40 && s[0].posSource == "DSC", "gap flagged %.1f", s.empty() ? 0 : s[0].posGapNm);
    // stale AIS (8 h) is not used
    IncidentBoard c;
    c.onAis(ship(244000111, 54.0, 12.0, 0, 0, "A"), 100);
    c.onDsc(distress("244000111", true, 54.7, 12.0), 100 + 8 * 3600);
    s = c.snapshot();
    CHECK(s.size() == 1 && !s[0].hasAisPos && s[0].posGapNm < 0, "stale AIS ignored");
    // no DSC position: the AIS one stands in
    IncidentBoard d;
    d.onAis(ship(244000111, 54.0, 12.0, 3, 90, "A"), 100);
    d.onDsc(distress("244000111", false, 0, 0), 200);
    s = d.snapshot();
    CHECK(s.size() == 1 && s[0].hasPos && s[0].posSource == "AIS" && near(s[0].lat, 54.0, 1e-6), "AIS position used");
    // and with neither, the incident still opens (distress must never be silent)
    IncidentBoard e;
    e.onDsc(distress("244000111", false, 0, 0), 200);
    s = e.snapshot();
    CHECK(s.size() == 1 && !s[0].hasPos && s[0].pinned && e.takeAlerts().size() == 1, "no position: still pinned");
    // a later AIS position fills it in
    e.onAis(ship(244000111, 54.0, 12.0, 0, 0, "A"), 260);
    s = e.snapshot();
    CHECK(s[0].hasPos && s[0].posSource == "AIS", "late AIS");
}

static void deviceTests() {
    // device first, DSC later: one incident
    IncidentBoard b;
    b.onAis(ship(970123456, 55.00, 11.00, 0, 0, "SART ACTIVE", AIS_CLASS_OTHER, 14), 100);
    auto s = b.snapshot();
    CHECK(s.size() == 1 && s[0].kind == "AIS-SART" && s[0].pinned && !s[0].test, "SART alone opens an incident");
    CHECK(b.takeAlerts().size() == 1, "SART alert");
    b.onDsc(distress("211222333", true, 55.01, 11.01), 160);
    s = b.snapshot();
    CHECK(s.size() == 1 && s[0].kind == "DSC distress" && s[0].devices.size() == 1 && s[0].devices[0] == "970123456", "SART folded into the DSC incident (%zu incidents)", s.size());
    // device after DSC
    IncidentBoard c;
    c.onDsc(distress("211222333", true, 55.01, 11.01), 100);
    c.takeAlerts();
    c.onAis(ship(972000007, 55.012, 11.012, 0, 0, "", AIS_CLASS_OTHER, 14), 200);
    s = c.snapshot();
    CHECK(s.size() == 1 && s[0].devices.size() == 1, "MOB joins the DSC incident");
    auto al = c.takeAlerts();
    CHECK(al.size() == 1 && al[0].what == "device", "device alert");
    // far device: its own incident
    c.onAis(ship(974000009, 56.5, 11.0, 0, 0, "", AIS_CLASS_OTHER, 14), 210);
    s = c.snapshot();
    CHECK(s.size() == 2, "far EPIRB is separate");
    // a test transmission neither pins nor alerts, and leaves after the keep time
    IncidentBoard t;
    t.onAis(ship(970999999, 55, 11, 0, 0, "SART TEST", AIS_CLASS_OTHER, 15), 100);
    s = t.snapshot();
    CHECK(s.size() == 1 && s[0].test && !s[0].pinned && t.takeAlerts().empty() && t.openCount() == 0, "test SART is quiet");
    t.onAis(ship(970999999, 55, 11, 0, 0, "SART TEST", AIS_CLASS_OTHER, 15), 200);
    t.tick(100 + 10000);
    CHECK(t.snapshot().empty(), "test record dropped");
    // a test device near a DSC incident does not join it
    IncidentBoard u;
    u.onDsc(distress("211222333", true, 55.0, 11.0), 100);
    u.onAis(ship(970999999, 55.001, 11.001, 0, 0, "SART TEST", AIS_CLASS_OTHER, 15), 120);
    s = u.snapshot();
    CHECK(s[0].kind == "DSC distress" || s.size() == 2, "test");
    bool joined = false;
    for (const auto& i : s) if (i.kind == "DSC distress" && !i.devices.empty()) joined = true;
    CHECK(!joined, "test device not joined");
    // a test turns active
    IncidentBoard v;
    v.onAis(ship(970999999, 55, 11, 0, 0, "SART TEST", AIS_CLASS_OTHER, 15), 100);
    v.onAis(ship(970999999, 55, 11, 0, 0, "", AIS_CLASS_OTHER, 14), 160);
    s = v.snapshot();
    CHECK(s.size() == 1 && !s[0].test && s[0].pinned && v.takeAlerts().size() == 1, "test -> active raises the alert");
}

static void navtexTests() {
    IncidentBoard b;
    b.onAis(ship(244123456, 54.8, 13.2, 0, 0, "ZEEVALK"), 100);
    b.onNavtex(nav("EA01", 'A', "Navigational warning", "LIGHT BUOY OUT OF POSITION 54-50N 013-30E."), 200);       // 18 nm away, before the incident
    b.onNavtex(nav("EA02", 'A', "Navigational warning", "CABLE LAYING WORK 58-00N 010-00E"), 210);                  // far
    b.onNavtex(nav("EA03", 'B', "Meteorological warning", "GALE WARNING BALTIC"), 220);                               // weather area: not joined
    DscCall c = distress("244123456", true, 54.8, 13.2);
    b.onDsc(c, 300);
    auto s = b.snapshot();
    CHECK(s.size() == 1 && s[0].navtex.size() == 1 && s[0].navtex[0].header == "EA01", "earlier NAVTEX with a position near: %zu", s.empty() ? 0 : s[0].navtex.size());
    b.onNavtex(nav("EA04", 'D', "Search and rescue information", "SAR OPERATION IN PROGRESS BALTIC SEA, ALL SHIPS KEEP WATCH CH16"), 400);   // area covers the scene
    b.onNavtex(nav("EA05", 'D', "Search and rescue information", "VESSEL 244123456 REPORTED SINKING, ZEEVALK"), 500);                   // names the MMSI
    b.onNavtex(nav("EA06", 'E', "Forecast", "FORECAST NORTH SEA"), 520);
    b.onNavtex(nav("EA04", 'D', "Search and rescue information", "SAR OPERATION IN PROGRESS BALTIC SEA, ALL SHIPS KEEP WATCH CH16"), 540);   // repeat of the same message
    s = b.snapshot();
    CHECK(s[0].navtex.size() == 3, "navtex joined: %zu", s[0].navtex.size());
    bool areaOk = false, mmsiOk = false;
    for (auto& n : s[0].navtex) { if (n.header == "EA04" && n.reason.find("Baltic") != std::string::npos) areaOk = true; if (n.header == "EA05" && n.reason.find("MMSI") != std::string::npos) mmsiOk = true; }
    CHECK(areaOk && mmsiOk, "reasons");
    // outside the time window: a NAVTEX message 3 days later is not added
    b.onNavtex(nav("EA07", 'D', "Search and rescue information", "VESSEL 244123456 STILL MISSING"), 300 + 3 * 86400);
    s = b.snapshot();
    CHECK(s[0].navtex.size() == 3, "late NAVTEX out of window");
}

// Sweep: random scenes, AIS ages, DSC position errors, damaged calls: the board must choose the right position and never invent an incident.
static void sweep() {
    std::mt19937 rng(11);
    int ok = 0, total = 0, flagged = 0, falseIncidents = 0, devOk = 0, devTotal = 0;
    for (int k = 0; k < 600; k++) {
        IncidentBoard b;
        std::uniform_real_distribution<double> la(-60, 70), lo(-179, 179), age(0, 8 * 3600), err(0, 0.6);
        const double sLat = la(rng), sLon = lo(rng);
        const double a = age(rng);
        const double dErr = err(rng) * (k % 3 == 0 ? 0.01 : 1.0);   // degrees of error between AIS and DSC
        const double dscLat = std::min(89.0, sLat + dErr), dscLon = sLon + dErr;
        b.onAis(ship(244555000 + k, sLat, sLon, 0, 0, "X"), 1000);
        // symbol-level: build, parse, use the parsed call
        const auto body = dscBuildDistress(std::to_string(244555000 + k), 101, dscLat, dscLon, 10, 10, 109);
        DscCall c = dscParseCall(body, 127, true, 0);
        c.eccOk = true;
        b.onDsc(c, 1000 + a);
        const auto s = b.snapshot();
        total++;
        if (s.size() == 1) {
            const Incident& i = s[0];
            const double want = incidentDistanceNm(dscLat, dscLon, i.lat, i.lon);
            if (want < 1.5 && i.posSource == "DSC") ok++;
            if (i.hasAisPos) { const double gap = incidentDistanceNm(sLat, sLon, dscLat, dscLon); if ((gap > 5) == i.posGapFlag) flagged++; }
            else flagged++;
        } else falseIncidents++;
        // a device near a ship far away never joins
        devTotal++;
        b.onAis(ship(970000000 + k, std::max(-89.0, std::min(89.0, sLat + 3)), sLon, 0, 0, "", AIS_CLASS_OTHER, 14), 1000 + a + 10);
        const auto s2 = b.snapshot();
        if (s2.size() == 2) devOk++;
    }
    printf("incident sweep: position right %d/%d, gap flag right %d/%d, wrong incident count %d, far devices kept apart %d/%d\n", ok, total, flagged, total, falseIncidents, devOk, devTotal);
    CHECK(ok == total && flagged == total && falseIncidents == 0 && devOk == devTotal, "sweep");
}

int main() {
    geoTests();
    dscScenario();
    gapTests();
    deviceTests();
    navtexTests();
    sweep();
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
