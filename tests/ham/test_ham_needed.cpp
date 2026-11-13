// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Worked-before and needed (4.0): WPX prefixes (portable forms), the needed levels over a small log (new DXCC, new band, new mode class, new
// zone, new state, new grid, new prefix, new call, B4, unconfirmed entity), the alert rules with their quiet time and the watch list.
#include "dect2/ham/ham_needed.h"
#include <cstdio>
#include <string>
using namespace dect2::ham;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static LogQso qso(const char* call, const char* band, const char* mode, int dxcc, const char* grid, int cqz, int ituz, const char* state, bool lotw) {
    LogQso q;
    q.call = call; q.band = band; q.mode = mode; q.dxcc = dxcc; q.grid = grid; q.cqz = cqz; q.ituz = ituz; q.state = state; q.lotwRcvd = lotw;
    return q;
}

int main() {
    // ---- WPX ----
    CHECK(wpxPrefix("LY1ABC") == "LY1" && wpxPrefix("ly1abc/p") == "LY1" && wpxPrefix("K1ABC/QRP") == "K1");
    CHECK(wpxPrefix("K1ABC/4") == "K4" && wpxPrefix("PJ4/K1ABC") == "PJ4" && wpxPrefix("DL/LY1ABC") == "DL0" && wpxPrefix("LY1ABC/DL") == "DL0");
    CHECK(wpxPrefix("RAEM") == "RA0" && wpxPrefix("3DA0RU") == "3DA0" && wpxPrefix("9A1ABC") == "9A1" && wpxPrefix("4U1ITU") == "4U1");
    CHECK(wpxPrefix("") == "" && wpxPrefix("K1-ABC") == "" && wpxPrefix("/P") == "");

    // ---- needed ----
    std::vector<LogQso> log = {qso("K1ABC", "20m", "FT8", 291, "FN42", 5, 8, "MA", true), qso("DL1XYZ", "40m", "CW", 230, "JO62", 14, 28, "", false)};
    NeedIndex n;
    n.build(log);
    CHECK(n.calls() == 2);
    auto q = [](const char* call, const char* band, const char* mode, const char* dk, const char* grid, int cqz, int ituz, const char* st) {
        NeedQuery x; x.call = call; x.band = band; x.mode = mode; x.dxccKey = dk; x.grid = grid; x.cqz = cqz; x.ituz = ituz; x.state = st; return x;
    };
    NeedResult r = n.check(q("K1ABC", "20m", "FT8", "291", "FN42", 5, 8, "MA"));
    CHECK(r.level == 0 && r.label == "B4" && r.bits == 0);
    r = n.check(q("K1ABC/P", "20m", "FT8", "291", "FN42", 5, 8, "MA"));   // portable: the same station
    CHECK(r.level == 0);
    r = n.check(q("K1ABC", "40m", "FT8", "291", "FN42", 5, 8, "MA"));
    CHECK(r.level == 5 && r.label == "new band" && r.has(NeedCallBand) && r.has(NeedDxccBand) && !r.has(NeedCall) && r.has(NeedGridBand));
    r = n.check(q("W1AW", "20m", "FT8", "291", "FN31", 5, 8, "CT"));
    CHECK(r.level == 3 && r.label == "new state" && r.has(NeedCall) && r.has(NeedGrid) && r.has(NeedState) && r.has(NeedPrefix) && !r.has(NeedDxcc));
    r = n.check(q("W1AW", "20m", "FT8", "291", "FN42", 5, 8, "MA"));
    CHECK(r.level == 2 && r.label == "new prefix");
    r = n.check(q("K1XYZ", "20m", "FT8", "291", "FN42", 5, 8, ""));
    CHECK(r.level == 1 && r.label == "new call");
    r = n.check(q("K1ABC", "20m", "FT4", "291", "FN42", 5, 8, "MA"));
    CHECK(r.level == 1 && r.label == "new slot" && r.has(NeedCallSlot));
    r = n.check(q("JA1ABC", "20m", "FT8", "339", "PM95", 25, 45, ""));
    CHECK(r.level == 6 && r.label == "NEW DXCC" && r.has(NeedCqz) && r.has(NeedItuz));
    r = n.check(q("DL2ABC", "40m", "SSB", "230", "JO62", 14, 28, ""));
    CHECK(r.level == 4 && r.label == "new mode" && r.has(NeedDxccQsl) && r.has(NeedDxccClass));
    r = n.check(q("DL1XYZ", "40m", "CW", "230", "JO62", 14, 28, ""));
    CHECK(r.level == 0 && r.has(NeedDxccQsl));                        // worked, not confirmed
    r = n.check(q("ZZ9ZZ", "20m", "FT8", "", "", 0, 0, ""));
    CHECK(!r.has(NeedDxcc) && r.level == 2 && r.label == "new prefix");   // the entity is not known: no entity marks
    r = n.check(q("", "20m", "FT8", "291", "", 0, 0, ""));
    CHECK(r.bits == 0 && r.label.empty());
    // a new QSO is counted at once
    n.add(qso("JA1ABC", "20m", "FT8", 339, "PM95", 25, 45, "", false));
    r = n.check(q("JA2XYZ", "20m", "FT8", "339", "PM95", 25, 45, ""));
    CHECK(r.level == 2 && r.label == "new prefix" && !r.has(NeedDxcc));

    // ---- alerts ----
    CHECK(wildMatch("VP8*", "vp8pj") && wildMatch("?Y0J", "3Y0J") && !wildMatch("K1*Z", "K1ABC") && wildMatch("*", "") && wildMatch("*/MM", "DL1ABC/MM"));
    NeedAlerter al;
    std::string why;
    const NeedQuery ja = q("JA9AAA", "15m", "FT8", "339", "PM95", 25, 45, "");
    n.build(log);
    CHECK(al.check(ja, n.check(ja), 1000, why) && why == "New DXCC: JA9AAA");
    CHECK(!al.check(ja, n.check(ja), 1500, why) && why.empty());       // quiet time
    CHECK(al.check(ja, n.check(ja), 1000 + 901, why));
    const NeedQuery b4 = q("K1ABC", "20m", "FT8", "291", "FN42", 5, 8, "MA");
    CHECK(!al.check(b4, n.check(b4), 5000, why));
    al.rules.watch = {"3Y0*", "K1ABC"};
    CHECK(al.check(b4, n.check(b4), 5000, why) && why == "Watched call: K1ABC");
    const NeedQuery st = q("W1AW", "20m", "FT8", "291", "FN31", 5, 8, "CT");
    CHECK(!al.check(st, n.check(st), 6000, why));                     // a new state alerts only when asked for
    al.rules.newState = true;
    CHECK(al.check(st, n.check(st), 6000, why) && why == "New state CT: W1AW");
    const NeedQuery band = q("DL5ZZZ", "20m", "FT8", "230", "JO62", 14, 28, "");
    CHECK(al.check(band, n.check(band), 7000, why) && why == "DXCC new on 20m: DL5ZZZ");

    printf(fails ? "ham_needed: %d FAILED\n" : "ham_needed: all passed\n", fails);
    return fails ? 1 : 0;
}
