// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham operating helpers: contest exchange, serials, contest dupes and the dupe sheet, Cabrillo export; the sun and the grayline,
// great-circle paths; band dial frequencies and region edges; the spots list; macros.
#include "dect2/ham/ham_ops.h"
#include "dect2/ham/ham.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2::ham;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    // ---- the sun ----
    double la, lo;
    subsolarPoint(1782043200.0, la, lo);               // 2026-06-21 12:00 UTC: near the June solstice, noon at Greenwich
    CHECK(std::fabs(la - 23.44) < 0.3 && std::fabs(lo) < 2.5);
    subsolarPoint(1797854400.0, la, lo);               // 2026-12-21 12:00 UTC
    CHECK(std::fabs(la + 23.44) < 0.3 && std::fabs(lo) < 2.5);
    subsolarPoint(1782043200.0 + 6 * 3600, la, lo);    // six hours later the sun is over 90 W
    CHECK(std::fabs(lo + 90) < 2.5);
    CHECK(sunElevation(23.44, 0, 1782043200.0) > 89);
    CHECK(sunElevation(-23.44, 180, 1782043200.0) < -89);
    const auto term = terminator(1782043200.0, 73);
    CHECK(term.size() == 73);
    int onLine = 0;
    for (const auto& p : term) onLine += std::fabs(sunElevation(p.first, p.second, 1782043200.0)) < 0.5;
    CHECK(onLine == 73);
    CHECK(inGrayline(term[10].first, term[10].second, 1782043200.0) && !inGrayline(23.44, 0, 1782043200.0));

    // ---- paths ----
    const auto path = gcPath(54.7, 25.3, 40.7, -74.0, 32);    // Vilnius - New York, over the North Atlantic
    CHECK(path.size() == 33 && std::fabs(path.front().first - 54.7) < 1e-6 && std::fabs(path.back().second + 74.0) < 1e-6);
    double maxLat = 0;
    for (const auto& p : path) maxLat = std::max(maxLat, p.first);
    CHECK(maxLat > 58);                                       // the great circle bends north of both ends
    const auto lp = gcPath(54.7, 25.3, 40.7, -74.0, 32, true);
    CHECK(lp.size() == 33 && std::fabs(lp.back().first - 40.7) < 1e-6 && std::fabs(lp.back().second + 74.0) < 1e-6);
    PathInfo pi;
    CHECK(pathBetween("KO24", "FN20", pi) && pi.km > 6500 && pi.km < 7500 && pi.bearing > 280 && pi.bearing < 320);
    CHECK(std::fabs(pi.km + pi.longKm - 40030.2) < 1 && std::fabs(std::fmod(pi.bearing + 180, 360) - pi.longBearing) < 1e-9);
    CHECK(!pathBetween("KO24", "ZZ99", pi));

    // ---- the band bar ----
    CHECK(dialFor("20m", 0) == 14.074 && dialFor("20m", 1) == 14.080 && dialFor("40m", 3) == 7.0386 && dialFor("60m", 1) == 0 && dialFor("3cm", 0) == 0);
    for (const auto& b : bandDials()) CHECK(b.ft8 > 0);
    CHECK(dialFor("11m", 0) == 27.265 && dialFor("11m", 1) == 0);   // 11 m (CB) has a receive dial only
    CHECK(rxBandNameFor(27.185e6) == "11m" && bandNameFor(27.185e6).empty() && rxBandNameFor(14.2e6) == "20m" && rxBandNameFor(26.9e6).empty() && rxBandNameFor(27.5e6).empty());
    CHECK(hamCbBand(26.965e6) && hamCbBand(27.405e6) && !hamCbBand(28.0e6));
    CHECK(regionAllows(1, 7.150e6, 2500) && !regionAllows(1, 7.250e6, 2500) && regionAllows(2, 7.250e6, 2500));
    CHECK(regionAllows(1, 70.2e6, 50) && !regionAllows(2, 70.2e6, 50) && !regionAllows(1, 147e6, 50) && regionAllows(2, 147e6, 50));
    CHECK(regionAllows(0, 1e9, 50));                         // no region chosen: no extra check

    // ---- contests ----
    Logbook log;
    ContestSession cs;
    for (int i = 0; i < (int)contestDefs().size(); i++) if (std::string(contestDefs()[(size_t)i].id) == "CQ-WPX-CW") cs.def = i;
    CHECK(cs.sent("599") == "599 001");
    auto logIt = [&](const char* call, double mhz, const char* mode, double utc, const std::string& rcvd) {
        LogQso q; q.call = call; q.freqHz = mhz * 1e6; q.mode = mode; q.startUtc = utc; q.rstSent = "599"; q.rstRcvd = "599";
        char b[8]; snprintf(b, sizeof b, "%03d", cs.nextSerial); q.stx = b; q.srx = rcvd; q.contestId = contestDefs()[(size_t)cs.def].id; q.myCall = "LY1ABC";
        log.add(q); cs.taken();
    };
    logIt("K1ABC", 14.025, "CW", 1792672496.0, "015");
    logIt("JA1XYZ", 14.030, "CW", 1792672556.0, "1234");
    CHECK(cs.sent("599") == "599 003");
    CHECK(cs.dupe(log, "K1ABC", "20m", "CW") && cs.dupe(log, "K1ABC/P", "20m", "CW") && !cs.dupe(log, "K1ABC", "40m", "CW"));
    LogQso other; other.call = "G4ABC"; other.freqHz = 14.02e6; other.mode = "CW"; other.startUtc = 1; log.add(other);   // not in the contest
    CHECK(!cs.dupe(log, "G4ABC", "20m", "CW"));
    const auto sheet = cs.sheet(log);
    CHECK(sheet.size() == 1 && sheet.at("20m").size() == 2 && sheet.at("20m")[0] == "JA1XYZ");
    ContestSession cs2; cs2.def = cs.def;
    cs2.resume(log);
    CHECK(cs2.nextSerial == 3);
    CabrilloHeader h; h.call = "ly1abc"; h.contest = "CQ-WPX-CW"; h.grid = "KO24";
    const std::string cab = cabrilloExport(log.qsos(), h);
    CHECK(cab.rfind("START-OF-LOG: 3.0\n", 0) == 0 && cab.find("CALLSIGN: LY1ABC\n") != std::string::npos && cab.find("CONTEST: CQ-WPX-CW\n") != std::string::npos);
    CHECK(cab.find("QSO: 14025 CW 2026-10-22 1234 LY1ABC        599 001    K1ABC         599 015\n") != std::string::npos);
    CHECK(cab.find("JA1XYZ        599 1234\n") != std::string::npos && cab.find("G4ABC") == std::string::npos);
    CHECK(cab.size() > 12 && cab.compare(cab.size() - 12, 12, "END-OF-LOG:\n") == 0);
    CHECK(cabrilloMode("USB") == "PH" && cabrilloMode("RTTY") == "RY" && cabrilloMode("FT8") == "DG" && cabrilloMode("cw") == "CW");
    ContestSession dg;
    for (int i = 0; i < (int)contestDefs().size(); i++) if (std::string(contestDefs()[(size_t)i].id) == "CQ-WW-DIGI") dg.def = i;
    dg.myExch = "ko24ab";
    CHECK(dg.sent("-10") == "KO24");
    ContestSession zn;
    for (int i = 0; i < (int)contestDefs().size(); i++) if (std::string(contestDefs()[(size_t)i].id) == "CQ-WW-CW") zn.def = i;
    zn.myExch = "15";
    CHECK(zn.sent("599") == "599 15");

    // ---- spots ----
    SpotList sp;
    sp.heard("K1ABC", "FN42", "20m", "FT8", 100, 14.074e6, 1200, -10, true, "CQ K1ABC FN42");
    sp.heard("JA1XYZ", "PM95", "20m", "FT8", 115, 14.074e6, 800, -18, true, "CQ JA1XYZ PM95");
    sp.heard("K1ABC", "", "20m", "FT8", 130, 14.074e6, 1210, -5, false, "LY1ABC K1ABC -05");
    CHECK(sp.spots().size() == 2 && sp.spots()[0].call == "K1ABC" && sp.spots()[0].count == 2 && sp.spots()[0].grid == "FN42" && sp.spots()[0].bestSnr == -5);
    sp.heard("K1ABC", "", "40m", "FT8", 140, 7.074e6, 1000, -7, true, "CQ K1ABC FN42");
    CHECK(sp.spots().size() == 3);
    sp.expire(130 + 1800 - 1, 1800);
    CHECK(sp.spots().size() == 2);

    // ---- macros ----
    MacroVars v; v.myCall = "LY1ABC"; v.myGrid = "KO24"; v.call = "K1ABC"; v.rst = "599"; v.serial = "007";
    CHECK(expandMacro("{CALL} DE {MYCALL} {rst} {SERIAL} {UNKNOWN} {", v) == "K1ABC DE LY1ABC 599 007 {UNKNOWN} {");

    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ham ops: all passed\n");
    return 0;
}
