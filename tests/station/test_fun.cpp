// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Surprise me, rare catches, the dossier and plain-English messages.
#include "dect2/station/fun.h"
#include <cstdio>
#include <map>
using namespace dect2;
using namespace dect2::station;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static msgdb::Record rec(double t, const char* mode, const char* ident, const char* extra = "") {
    msgdb::Record r; r.time = t; r.mode = mode; r.type = "x.m"; r.ident = ident; r.extra = extra; return r;
}

static void testSurprise() {
    CHECK(surprise({}, 1) == -1);
    CHECK(surprise({{false, false}, {false, true}}, 1) == -1);
    CHECK(surprise({{false, false}, {true, false}, {false, false}}, 12345) == 1);
    // the same seed gives the same choice; the weights hold over many seeds
    std::vector<SurpriseMode> m = {{true, true}, {true, false}, {false, false}};
    CHECK(surprise(m, 7) == surprise(m, 7));
    int count[3] = {};
    for (uint32_t s = 0; s < 40000; s++) { int r = surprise(m, s); CHECK(r >= 0 && r < 2); if (r >= 0) count[r]++; }
    CHECK(count[2] == 0);
    const double ratio = (double)count[1] / count[0];
    printf("  unheard / heard picks: %.2f\n", ratio);
    CHECK(ratio > 2.5 && ratio < 3.5);
}

static void testRare() {
    RareWatcher w;
    w.setKnownModes({"ADS-B", "AIS"});
    RareCatch c;
    CHECK(!w.check(rec(1, "ADS-B", "4CA123", "squawk=1000\nalt_ft=30000"), c));
    CHECK(w.check(rec(2, "ADS-B", "4CA123", "squawk=7700"), c));
    CHECK(c.kind == RareKind::EmergencySquawk); CHECK(c.text.find("7700") != std::string::npos);
    CHECK(!w.check(rec(3, "ADS-B", "4CA123", "squawk=7700"), c));       // once per aircraft
    CHECK(w.check(rec(4, "ADS-B", "4CA124", "squawk=7600"), c));        // another aircraft does alert
    CHECK(w.check(rec(5, "AIS", "970123456"), c)); CHECK(c.kind == RareKind::SartBeacon);
    CHECK(!w.check(rec(6, "AIS", "970123456"), c));
    CHECK(w.check(rec(7, "AIS", "972123456"), c)); CHECK(c.kind == RareKind::SartBeacon);
    CHECK(w.check(rec(8, "AIS", "974123456"), c)); CHECK(c.kind == RareKind::SartBeacon);
    CHECK(!w.check(rec(9, "AIS", "277123456"), c));                     // an ordinary ship
    // the first message of a mode that was not in the history, once, whatever the case
    CHECK(w.check(rec(10, "ACARS", "X"), c)); CHECK(c.kind == RareKind::FirstOfMode);
    CHECK(!w.check(rec(11, "acars", "Y"), c));
    CHECK(!w.check(rec(12, "", "Z"), c));
}

static void testDossier() {
    msgdb::Db db; std::string err;
    CHECK(db.open(":memory:", &err));
    msgdb::Record r = rec(100, "ADS-B", "A00001"); r.freqHz = 1090e6; r.lat = 40; r.lon = -74; r.hasPos = true; r.text = "first"; CHECK(db.add(r));
    r = rec(300, "ACARS", "a00001"); r.freqHz = 131.725e6; r.text = "OOOI"; CHECK(db.add(r));
    r = rec(200, "ADS-B", "A00001"); r.freqHz = 1090e6; r.text = "again"; CHECK(db.add(r));
    r = rec(500, "ADS-B", "AA00001"); r.freqHz = 1090e6; CHECK(db.add(r));                 // only contains the name
    Dossier d = dossier(db, "A00001");
    CHECK(d.kind == QueryKind::Icao); CHECK(d.messages == 3); CHECK(d.first == 100); CHECK(d.last == 300);
    CHECK(d.modes.size() == 2); CHECK(d.freqsHz.size() == 2); CHECK(d.freqsHz[0] == 131.725e6);
    CHECK(d.hasPos); CHECK(d.lat == 40); CHECK(d.recent.size() == 3); CHECK(d.recent[0].text == "OOOI");
    CHECK(d.country == "United States"); CHECK(d.registration == "N1");
    r = rec(10, "AIS", "277123456"); r.freqHz = 162.025e6; CHECK(db.add(r));
    d = dossier(db, "277123456");
    CHECK(d.kind == QueryKind::Mmsi); CHECK(!d.country.empty()); CHECK(d.what.size() > 0); CHECK(d.messages == 1);
    d = dossier(db, "SP5ABC"); CHECK(d.messages == 0); CHECK(d.country.empty());
}

static void testPlain() {
    CHECK(plainEnglish(rec(1, "ADS-B", "4CA123", "squawk=7500")).find("hijacking") != std::string::npos);
    CHECK(plainEnglish(rec(1, "AIS", "277123456", "nav_status=1\nsog_kt=0")) == "This ship says it is at anchor.");
    CHECK(plainEnglish(rec(1, "AIS", "277123456", "nav_status=7")).find("fishing") != std::string::npos);
    CHECK(plainEnglish(rec(1, "AIS", "277123456", "nav_status=14")).find("distress") != std::string::npos);
    // an undefined status says nothing about the status, the MMSI rule answers instead
    CHECK(plainEnglish(rec(1, "AIS", "277123456", "nav_status=15")).find("says it is") == std::string::npos);
    CHECK(plainEnglish(rec(1, "AIS", "277123456", "")).find("registered in") != std::string::npos);
    CHECK(plainEnglish(rec(1, "ADS-B", "A00001", "squawk=1200")).find("United States") != std::string::npos);
    // nothing certain to say: nothing is said
    CHECK(plainEnglish(rec(1, "POCSAG", "1234567", "text=hello")).empty());
    CHECK(plainEnglish(rec(1, "APRS", "LY2XX-9", "")).empty());
    CHECK(plainEnglish(rec(1, "APRS", "", "")).empty());
}

int main() {
    testSurprise(); testRare(); testDossier(); testPlain();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("fun ok\n");
    return 0;
}
