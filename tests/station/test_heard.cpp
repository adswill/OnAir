// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Search everything heard: the query parser and the grouping over the message database.
#include "dect2/station/heard.h"
#include "dect2/track/track_hub.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::station;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void testParse() {
    Query q = parseQuery("  162.025 ");
    CHECK(q.kind == QueryKind::Frequency); CHECK(std::fabs(q.loHz - 162.02e6) < 1); CHECK(std::fabs(q.hiHz - 162.03e6) < 1);
    q = parseQuery("162.025 MHz"); CHECK(q.kind == QueryKind::Frequency); CHECK(std::fabs((q.loHz + q.hiHz) / 2 - 162.025e6) < 1);
    q = parseQuery("162025 kHz"); CHECK(q.kind == QueryKind::Frequency); CHECK(std::fabs((q.loHz + q.hiHz) / 2 - 162.025e6) < 1);
    q = parseQuery("1090mhz"); CHECK(q.kind == QueryKind::Frequency); CHECK(std::fabs((q.loHz + q.hiHz) / 2 - 1090e6) < 1);
    q = parseQuery("14.074"); CHECK(q.kind == QueryKind::Frequency);
    q = parseQuery("144-146"); CHECK(q.kind == QueryKind::Frequency); CHECK(q.loHz == 144e6); CHECK(q.hiHz == 146e6);
    q = parseQuery("144 - 146 MHz"); CHECK(q.kind == QueryKind::Frequency); CHECK(q.hiHz == 146e6);
    q = parseQuery("7000-7200 kHz"); CHECK(q.kind == QueryKind::Frequency); CHECK(q.loHz == 7e6); CHECK(q.hiHz == 7.2e6);
    CHECK(parseQuery("").kind == QueryKind::Empty);
    CHECK(parseQuery("   ").kind == QueryKind::Empty);
    CHECK(parseQuery("4CA123").kind == QueryKind::Icao);
    CHECK(parseQuery("4ca123").kind == QueryKind::Icao);
    CHECK(parseQuery("277123456").kind == QueryKind::Mmsi);
    CHECK(parseQuery("SP5ABC").kind == QueryKind::Callsign);
    CHECK(parseQuery("LY2XX-9").kind == QueryKind::Callsign);
    CHECK(parseQuery("9A1AA").kind == QueryKind::Callsign);
    CHECK(parseQuery("hello world").kind == QueryKind::Text);
    CHECK(parseQuery("Ryanair").kind == QueryKind::Text);
    CHECK(parseQuery("12345").kind == QueryKind::Text);    // a bare number is not a frequency
    CHECK(parseQuery("1.2.3").kind != QueryKind::Frequency);
    CHECK(parseQuery("100-50").kind != QueryKind::Frequency);
}

static msgdb::Record rec(double t, const char* mode, const char* ident, double f, const char* text) {
    msgdb::Record r; r.time = t; r.mode = mode; r.type = "x.message"; r.ident = ident; r.freqHz = f; r.text = text; return r;
}

static void testSearch() {
    msgdb::Db db;
    std::string err;
    CHECK(db.open(":memory:", &err));
    msgdb::Record r = rec(100, "ADS-B", "4CA123", 1090e6, "RYR1AB FL350");
    r.lat = 54.7; r.lon = 25.3; r.hasPos = true; CHECK(db.add(r));
    CHECK(db.add(rec(200, "ADS-B", "4CA123", 1090e6, "RYR1AB FL360")));
    CHECK(db.add(rec(300, "ACARS", "4CA123", 131.725e6, "OOOI OUT VNO")));
    CHECK(db.add(rec(150, "VDL2", "4ca123", 136.975e6, "same plane lower case")));
    CHECK(db.add(rec(120, "AIS", "277123456", 162.025e6, "MS LITHUANIA at anchor")));
    CHECK(db.add(rec(130, "AIS", "277999999", 162.025e6, "MS OTHER mentions 277123456 in text")));
    CHECK(db.add(rec(140, "APRS", "LY2XX-9", 144.8e6, "mobile")));
    CHECK(db.add(rec(141, "APRS", "LY2XX-1", 144.8e6, "fixed")));
    CHECK(db.add(rec(142, "POCSAG", "", 148.5e6, "pager without identity")));

    std::vector<Heard> h = searchHeard(db, "4ca123");
    CHECK(h.size() == 1);
    if (!h.empty()) {
        CHECK(h[0].count == 4); CHECK(h[0].modes.size() == 3); CHECK(h[0].first == 100); CHECK(h[0].last == 300);
        CHECK(h[0].lastFreqHz == 131.725e6); CHECK(h[0].lastText == "OOOI OUT VNO");
        CHECK(h[0].hasPos); CHECK(!h[0].live); CHECK(h[0].kind == QueryKind::Icao);
    }
    // the exact identity comes first, the one that only mentions it in its text after it, though that one was heard later
    h = searchHeard(db, "277123456");
    CHECK(h.size() == 2);
    if (h.size() == 2) { CHECK(h[0].ident == "277123456"); CHECK(h[0].kind == QueryKind::Mmsi); CHECK(h[1].ident == "277999999"); }
    // a part of a call sign finds both SSIDs, each its own row, newest first
    h = searchHeard(db, "LY2XX");
    CHECK(h.size() == 2);
    if (h.size() == 2) { CHECK(h[0].ident == "LY2XX-1"); CHECK(h[1].ident == "LY2XX-9"); }
    // frequency: both ships, nothing else
    h = searchHeard(db, "162.025");
    CHECK(h.size() == 2);
    // a range finds APRS and the pager, which has no identity and is grouped by mode
    h = searchHeard(db, "144-149");
    CHECK(h.size() == 3);
    bool none = false;
    for (const Heard& x : h) if (x.ident.empty()) { none = true; CHECK(x.modes.size() == 1 && x.modes[0] == "POCSAG"); }
    CHECK(none);
    // free text
    h = searchHeard(db, "anchor"); CHECK(h.size() == 1);
    CHECK(searchHeard(db, "nothinglikethis").empty());
    CHECK(searchHeard(db, "   ").empty());
    // a live track wins over the stored position
    h = searchHeard(db, "4CA123", [](const std::string& id, double& la, double& lo) { if (id != "4CA123" && id != "4ca123") return false; la = 1; lo = 2; return true; });
    CHECK(h.size() == 1);
    if (!h.empty()) { CHECK(h[0].live); CHECK(h[0].lat == 1); CHECK(h[0].lon == 2); }
    // the row limit applies to the records looked at
    h = searchHeard(db, "4CA123", nullptr, 1);
    CHECK(h.size() == 1);
    if (!h.empty()) CHECK(h[0].count == 1);
}

static void testLiveFix() {
    namespace tk = dect2::track;
    tk::TrackHub older, newer;
    tk::registerHub(&older); tk::registerHub(&newer);
    const tk::Key k = tk::keyFromIcao(0x4CA123);
    double la = 0, lo = 0;
    CHECK(!tk::liveFix(k, la, lo));
    tk::Fix f; f.src = tk::Src::Other; f.hasPos = true; f.lat = 54.0; f.lon = 25.0; f.posSigmaM = 50; f.t = 100;
    older.feed(k, f);
    CHECK(tk::liveFix(k, la, lo)); CHECK(std::fabs(la - 54.0) < 0.05); CHECK(std::fabs(lo - 25.0) < 0.05);
    f.t = 200; f.lat = 55.0; f.lon = 26.0;
    newer.feed(k, f);
    CHECK(tk::liveFix(k, la, lo)); CHECK(std::fabs(la - 55.0) < 0.05); CHECK(std::fabs(lo - 26.0) < 0.05);
    tk::unregisterHub(&newer);
    CHECK(tk::liveFix(k, la, lo)); CHECK(std::fabs(la - 54.0) < 0.05);
    tk::unregisterHub(&older);
    CHECK(!tk::liveFix(k, la, lo));
    CHECK(!tk::liveFix(tk::keyFromIcao(0x123456), la, lo));
}

int main() {
    testParse(); testSearch(); testLiveFix();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("heard ok\n");
    return 0;
}
