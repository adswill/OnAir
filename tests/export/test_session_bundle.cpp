// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Export this session as one bundle": what goes into the zip, what stays out (credentials, location unless asked).
#include "../../app/state/session_bundle.h"
#include "dect2/backup/backup.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

namespace bk = dect2::backup;

static const bk::Entry* find(const std::vector<bk::Entry>& v, const std::string& n) {
    for (const auto& e : v) if (e.name == n) return &e;
    return nullptr;
}

static sessionbundle::Input sample() {
    sessionbundle::Input in;
    in.version = "4.6.0"; in.mode = "ADS-B"; in.freqHz = 1090e6; in.sampleRateHz = 2e6; in.device = "HackRF One";
    in.t0 = 1700000000; in.t1 = 1700003600;
    in.log.cols = {"time_utc", "table", "call", "lat", "lon"};
    in.log.add({"2023-11-14 22:13:20", "adsb", "ABC123", "52.5", "13.4"}, 1700000000);
    in.log.add({"2023-11-14 22:14:00", "adsb", "no,pos", "", ""}, 1700000040);
    in.recordings.push_back({"a.wav", 1234, 1700000100});
    in.recordings.push_back({"b,c.iq", 99, 1700000200});
    in.prefs = {{"theme", 'S', "dark"}, {"qrzPassword", 'S', "hunter2"}, {"adsbLat", 'D', "52.5"}};
    in.hasLocation = true; in.lat = 52.5; in.lon = 13.4; in.grid = "JO62";
    return in;
}

int main() {
    // 1. all parts present, a valid zip that unpacks again
    {
        sessionbundle::Input in = sample();
        in.withLocation = true;
        std::string zip = sessionbundle::build(in);
        std::vector<bk::Entry> es;
        CHECK(bk::zipUnpack(zip, es).empty());
        CHECK(find(es, "log.csv") && find(es, "log.kml") && find(es, "log.gpx"));
        CHECK(find(es, "recordings.csv") && find(es, "settings.txt") && find(es, "radio.txt") && find(es, "location.txt") && find(es, "summary.txt"));
        CHECK(find(es, "log.csv")->data.find("ABC123") != std::string::npos);
        CHECK(find(es, "log.kml")->data.find("<coordinates>13.4,52.5") != std::string::npos);
        CHECK(find(es, "recordings.csv")->data.find("\"b,c.iq\"") != std::string::npos);   // quoting
        CHECK(find(es, "radio.txt")->data.find("1090") != std::string::npos);
        CHECK(find(es, "summary.txt")->data.find("2 rows") != std::string::npos || find(es, "summary.txt")->data.find("2 decoded") != std::string::npos);
    }
    // 2. credentials never in the settings file
    {
        sessionbundle::Input in = sample();
        std::vector<bk::Entry> es; bk::zipUnpack(sessionbundle::build(in), es);
        const bk::Entry* s = find(es, "settings.txt");
        CHECK(s && s->data.find("theme") != std::string::npos);
        CHECK(s && s->data.find("hunter2") == std::string::npos);
        for (const auto& e : es) CHECK(e.data.find("hunter2") == std::string::npos);
    }
    // 3. location only when asked
    {
        sessionbundle::Input in = sample();
        in.withLocation = false;
        std::vector<bk::Entry> es; bk::zipUnpack(sessionbundle::build(in), es);
        CHECK(!find(es, "location.txt"));
        for (const auto& e : es) if (e.name != "log.csv" && e.name != "log.kml" && e.name != "log.gpx") CHECK(e.data.find("JO62") == std::string::npos);
        CHECK(find(es, "settings.txt")->data.find("adsbLat") == std::string::npos);   // the position pref is left out too
    }
    // 4. options leave parts out; empty log still gives a log header and no KML/GPX
    {
        sessionbundle::Input in = sample();
        in.withRecordings = false; in.withSettings = false;
        in.log.rows.clear(); in.log.epoch.clear();
        std::vector<bk::Entry> es; bk::zipUnpack(sessionbundle::build(in), es);
        CHECK(!find(es, "recordings.csv") && !find(es, "settings.txt"));
        CHECK(find(es, "log.csv") && !find(es, "log.kml") && !find(es, "log.gpx"));
        CHECK(find(es, "summary.txt")->data.find("0 rows") != std::string::npos || find(es, "summary.txt")->data.find("0 decoded") != std::string::npos);
    }
    // 5. file name
    CHECK(sessionbundle::fileName(1700000000) == "onair-session-20231114-221320.zip");
    printf(fails ? "FAILED\n" : "all ok\n");
    return fails ? 1 : 0;
}
