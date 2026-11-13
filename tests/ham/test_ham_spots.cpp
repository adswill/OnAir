// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spots from outside the station (4.0): DX cluster and RBN lines (time on the right day, the spotter's locator, the RBN SNR and speed), the
// mode guess, POTA / SOTA JSON (escapes, nulls, nested values, bad rows), PSK Reporter XML, the split a DX asks for, and the spot book merge.
// Real answers of the services were not fetched for this test: the samples follow their documented field names.
#include "dect2/ham/ham_spots.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2::ham;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    const double day = 1794441600.0;   // 2026-11-12 00:00 UTC

    // ---- cluster lines ----
    {
        ExtSpot s;
        CHECK(parseClusterLine("DX de W3LPL:     14025.0  JA1ABC       CW 599 up 2                 1234Z FN20\r\n", day + 12 * 3600 + 40 * 60, s));
        CHECK(s.src == SpotSrc::Cluster && s.spotter == "W3LPL" && s.call == "JA1ABC" && std::fabs(s.freqHz - 14025000) < 1e-3);
        CHECK(s.mode == "CW" && s.comment == "CW 599 up 2" && s.spotterGrid == "FN20" && s.utc == day + 12 * 3600 + 34 * 60);
        CHECK(s.band() == "20m");
        double tx = 0;
        CHECK(parseSplit(s.comment, s.freqHz, tx) && std::fabs(tx - 14027000) < 1e-3);

        CHECK(parseClusterLine("DX de DK8NE-#:   14025.0  JA1ABC       CW    12 dB  25 WPM  CQ      1234Z", day + 13 * 3600, s));
        CHECK(s.src == SpotSrc::Rbn && s.spotter == "DK8NE" && s.snr == 12 && s.wpm == 25 && s.mode == "CW");

        CHECK(parseClusterLine("DX de OH2BH:  7074.5 K1ABC  -12 dB 2355Z", day + 300, s));      // just after midnight: yesterday's spot
        CHECK(s.utc == day - 300 && s.mode == "FT8");                                             // 7.0745 MHz: 500 Hz above the FT8 dial
        CHECK(parseClusterLine("DX de G3XYZ: 14200.0 VK2ABC 59 tnx 1000Z", day + 11 * 3600, s) && s.mode == "SSB");
        CHECK(parseClusterLine("DX de G3XYZ: 14080.0 VK2ABC rtty contest 1000Z", day + 11 * 3600, s) && s.mode == "RTTY");
        CHECK(!parseClusterLine("Hello W3LPL, welcome to the cluster", day, s));
        CHECK(!parseClusterLine("DX de X: abc JA1ABC 1200Z", day, s));
        CHECK(!parseClusterLine("DX de X: 14025.0 ABC 1200Z", day, s));   // not a call
        CHECK(!parseClusterLine("DX de X: 14025.0 JA1ABC CW 2460Z", day, s));
        CHECK(spotModeGuess("", 3.530e6, 1) == "CW" && spotModeGuess("usb", 3.530e6, 1) == "SSB" && spotModeGuess("PSK", 14.070e6, 1) == "PSK31");
        CHECK(spotModeGuess("", 145.5e6, 1) == "FM" && spotModeGuess("", 14.072e6, 1) == "");
    }

    // ---- time ----
    CHECK(isoUtc("2026-11-12T10:15:30") == 1794478530.0 && isoUtc("2026-11-12T10:15:30Z") == 1794478530.0);
    CHECK(isoUtc("2026-11-12 10:15:30.123") == 1794478530.0 && isoUtc("2026-11-12T10:15") == 1794478500.0);
    CHECK(isoUtc("2000-01-01T00:00:00") == 946684800.0 && isoUtc("bad") == 0 && isoUtc("2026-13-12T10:15:30") == 0);

    // ---- POTA ----
    {
        const std::string j = R"([{"spotId":1,"activator":"K1ABC","frequency":"14062","mode":"CW","reference":"US-0001","parkName":null,
            "spotTime":"2026-11-12T10:15:30","spotter":"W2XYZ","comments":"QRT soon \"tnx\" é","name":"Acadia National Park","grid4":"FN54",
            "grid6":"FN54ab","nested":{"a":[1,2,{"b":"c"}]},"invalid":false},
            {"activator":"","frequency":"7"},
            {"activator":"N0CALL","frequency":"7074.5","mode":"FT8","reference":"US-1234","name":"Park","spotTime":"2026-11-12T10:16:00","grid4":"EN34"}])";
        const auto v = parsePotaSpots(j);
        CHECK(v.size() == 2);
        if (v.size() == 2) {
            CHECK(v[0].call == "K1ABC" && v[0].ref == "US-0001" && v[0].refName == "Acadia National Park" && v[0].mode == "CW");
            CHECK(std::fabs(v[0].freqHz - 14062000) < 1e-3 && v[0].utc == 1794478530.0 && v[0].grid == "FN54ab" && v[0].spotter == "W2XYZ");
            CHECK(v[0].comment == "QRT soon \"tnx\" \xc3\xa9" && v[0].src == SpotSrc::Pota);
            CHECK(v[1].call == "N0CALL" && v[1].grid == "EN34" && std::fabs(v[1].freqHz - 7074500) < 1e-3);
        }
        CHECK(parsePotaSpots("").empty() && parsePotaSpots("{\"x\":1}").empty() && parsePotaSpots("[{\"activator\":\"K1ABC\",").empty());
    }
    // ---- SOTA ----
    {
        const std::string j = R"([{"id":1,"userID":2,"timeStamp":"2026-11-12T09:00:00.123","comments":"cw","callsign":"G4XYZ","associationCode":"W7W",
            "summitCode":"LC-001","activatorCallsign":"n7abc/p","activatorName":"Al","frequency":"14.062","mode":"cw",
            "summitDetails":"Some Peak, 1234m, 8 Points","highlightColor":null}])";
        const auto v = parseSotaSpots(j);
        CHECK(v.size() == 1);
        if (!v.empty()) {
            CHECK(v[0].call == "N7ABC/P" && std::fabs(v[0].freqHz - 14062000) < 1e-3 && v[0].mode == "CW" && v[0].ref == "W7W/LC-001");
            CHECK(v[0].spotter == "G4XYZ" && v[0].utc == day + 9 * 3600 && v[0].refName.find("Some Peak") == 0 && v[0].src == SpotSrc::Sota);
        }
    }
    // ---- PSK Reporter ----
    {
        const std::string x = R"(<?xml version="1.0" encoding="UTF-8"?><receptionReports currentSeconds="1794441700">
            <activeReceiver callsign="X1X" locator="AA00"/>
            <receptionReport receiverCallsign="DL1XYZ" receiverLocator="JO62qm" senderCallsign="LY1ABC" senderLocator="KO24" frequency="14075879"
              flowStartSeconds="1794440000" mode="FT8" sNR="-12" />
            <receptionReport receiverCallsign="K1ABC" senderCallsign="LY1ABC" frequency="14075900" flowStartSeconds="1794440100" mode="FT8" sNR="-20"/>
            <receptionReport receiverCallsign="" senderCallsign="LY1ABC" frequency="14075900"/>
            </receptionReports>)";
        const auto v = parsePskReporterXml(x);
        CHECK(v.size() == 2);
        if (v.size() == 2) {
            CHECK(v[0].spotter == "K1ABC" && v[0].snr == -20 && v[0].utc == 1794440100.0);
            CHECK(v[1].spotter == "DL1XYZ" && v[1].spotterGrid == "JO62qm" && v[1].grid == "KO24" && std::fabs(v[1].freqHz - 14075879) < 1e-3);
        }
        CHECK(parsePskReporterXml("not xml").empty());
    }

    // ---- splits ----
    {
        double tx = 0;
        CHECK(parseSplit("listening UP 5-10", 14.195e6, tx) && std::fabs(tx - 14.2025e6) < 1e-3);
        CHECK(parseSplit("QSX 14.205", 14.195e6, tx) && std::fabs(tx - 14.205e6) < 1e-3);
        CHECK(parseSplit("qsx 205", 14.195e6, tx) && std::fabs(tx - 14.205e6) < 1e-3);
        CHECK(parseSplit("QSX 14205", 14.195e6, tx) && std::fabs(tx - 14.205e6) < 1e-3);
        CHECK(parseSplit("dwn 3", 7.010e6, tx) && std::fabs(tx - 7.007e6) < 1e-3);
        CHECK(parseSplit("UP", 7.010e6, tx) && std::fabs(tx - 7.011e6) < 1e-3);
        CHECK(parseSplit("pse UP2", 7.010e6, tx) && std::fabs(tx - 7.012e6) < 1e-3);
        CHECK(!parseSplit("UP0L de UN7XX", 7.010e6, tx));    // a Kazakh prefix, not a split
        CHECK(!parseSplit("CQ DX", 7.010e6, tx) && !parseSplit("UPDATE", 7.010e6, tx) && !parseSplit("", 7.010e6, tx));
        CHECK(parseSplit("UP 14.025", 14.020e6, tx) && std::fabs(tx - 14.025e6) < 1e-3);   // a frequency after UP
        CHECK(!parseSplit("UP 99", 14.020e6, tx));          // more than 50 kHz: not a split amount
    }

    // ---- the spot book ----
    {
        SpotBook b;
        ExtSpot s;
        s.src = SpotSrc::Cluster; s.call = "JA1ABC"; s.freqHz = 14025000; s.mode = "CW"; s.utc = day + 100; s.spotter = "A1AA";
        b.add(s);
        s.utc = day + 200; s.spotter = "B1BB"; s.freqHz = 14025100;
        b.add(s);
        CHECK(b.rows().size() == 1 && b.rows()[0].spotters == 2 && b.rows()[0].s.utc == day + 200 && b.rows()[0].s.freqHz == 14025100);
        s.utc = day + 150; s.spotter = "C1CC";
        b.add(s);                                    // older than the row: merged into it, nothing changes
        CHECK(b.rows().size() == 1 && b.rows()[0].s.utc == day + 200);
        ExtSpot r = s; r.src = SpotSrc::Rbn; r.utc = day + 250; r.spotter = "DK8NE";
        b.add(r);
        ExtSpot f = s; f.freqHz = 7025000; f.utc = day + 300;
        b.add(f);
        ExtSpot p = s; p.src = SpotSrc::Pota; p.call = "K1ABC"; p.mode = "SSB"; p.freqHz = 14250000; p.utc = day + 50;
        b.add(p);
        CHECK(b.rows().size() == 4 && b.rows()[0].s.freqHz == 7025000 && b.rows()[3].s.call == "K1ABC");   // newest first
        CHECK(b.filter("20m", "", 0xFF).size() == 3 && b.filter("", "CW", 1u << (int)SpotSrc::Rbn).size() == 1 && b.filter("", "ssb", 0xFF).size() == 1);
        CHECK(b.filter("", "", 0).empty());
        ExtSpot bad = s; bad.call = "";
        b.add(bad);
        CHECK(b.rows().size() == 4);
        b.expire(day + 50 + 1800 + 1);              // the POTA spot is older than 30 minutes
        CHECK(b.rows().size() == 3);
        b.expire(day + 300 + 1801);
        CHECK(b.rows().empty());
    }

    printf(fails ? "ham_spots: %d FAILED\n" : "ham_spots: all passed\n", fails);
    return fails ? 1 : 0;
}
