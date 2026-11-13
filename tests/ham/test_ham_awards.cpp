// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham awards and lookups: the DXCC / WAS / grid / CQ-zone matrix (worked and confirmed per band and mode class), statistics, the
// worked-before marks of the decode list, the built-in prefix database and a user cty.dat file.
#include "dect2/ham/ham_dxcc.h"
#include "dect2/ham/ham_log.h"
#include <cstdio>
#include <string>
using namespace dect2::ham;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static LogQso q(const char* call, const char* band, const char* mode, int dxcc, const char* state, const char* grid, int cqz, bool conf) {
    LogQso r; r.call = call; r.band = band; r.mode = mode; r.dxcc = dxcc; r.state = state; r.grid = grid; r.cqz = cqz; r.lotwRcvd = conf; r.startUtc = 1792672496.0;
    return r;
}

int main() {
    // ---- the prefix database ----
    const DxccDb& db = DxccDb::builtin();
    CHECK(db.entities().size() > 180 && db.prefixCount() > 600);
    auto ent = [&db](const char* c) { const DxccHit h = db.lookup(c); return h.e ? h.e->name : std::string("-"); };
    CHECK(ent("LY1ABC") == "Lithuania" && db.lookup("LY1ABC").e->adif == 146 && db.lookup("LY1ABC").cqz == 15);
    CHECK(ent("K1ABC") == "United States" && ent("W6XYZ") == "United States" && ent("AA1A") == "United States");
    CHECK(db.lookup("W6XYZ").cqz == 3 && db.lookup("K1ABC").cqz == 5);       // a zone override by call area
    CHECK(ent("KH6ABC") == "Hawaii" && ent("KL7XX") == "Alaska" && ent("KP4AA") == "Puerto Rico");
    CHECK(ent("VE3ABC") == "Canada" && ent("JA1XYZ") == "Japan" && ent("VK2ABC") == "Australia" && ent("ZL1AA") == "New Zealand");
    CHECK(ent("RA3AA") == "European Russia" && ent("UA9AA") == "Asiatic Russia" && ent("RW0AA") == "Asiatic Russia" && ent("RA2FA") == "Kaliningrad");
    CHECK(ent("UT2AB") == "Ukraine" && ent("UR5ABC") == "Ukraine" && ent("EW1AA") == "Belarus");
    CHECK(ent("GM3ABC") == "Scotland" && ent("M0ABC") == "England" && ent("2E0ABC") == "England" && ent("MW0AAA") == "Wales");
    CHECK(ent("IS0ABC") == "Sardinia" && ent("IK2ABC") == "Italy" && ent("EA8AA") == "Canary Islands" && ent("EA1AA") == "Spain");
    CHECK(ent("DL/LY1ABC") == "Germany" && ent("LY1ABC/DL") == "Germany" && ent("LY1ABC/P") == "Lithuania" && ent("K1ABC/KH6") == "Hawaii");
    CHECK(ent("K1ABC/4") == "United States" && db.lookup("K1ABC/6").cqz == 3);
    CHECK(!db.lookup("LY1ABC/MM").e && !db.lookup("").e && !db.lookup("Q1AAA").e);
    CHECK(prefixPart("DL/LY1ABC/P") == "DL" && prefixPart("k1abc/4") == "K4" && prefixPart("LY1ABC/QRP") == "LY1ABC");
    CHECK(db.byAdif(291) && db.byAdif(291)->name == "United States" && !db.byAdif(0));
    {   // every entity's primary prefix finds that entity
        int bad = 0;
        for (const auto& e : db.entities()) { const DxccHit h = db.lookup(e.primary + "1AA"); if (!h.e || h.e->name != e.name) { if (bad++ < 5) printf("  primary %s -> %s\n", e.primary.c_str(), h.e ? h.e->name.c_str() : "-"); } }
        CHECK(bad == 0);
        int noNum = 0;
        for (const auto& e : db.entities()) noNum += e.adif <= 0;
        CHECK(noNum == 0);
    }
    // a user cty.dat (the real file's layout: west-positive longitude, exact calls, zone overrides, '*' entity)
    {
        const std::string cty =
            "Sov Mil Order of Malta:   15:  28:  EU:   41.90:   -12.43:    -1.0:  1A:\n    1A;\n"
            "United States:            05:  08:  NA:   37.53:    91.67:     5.0:  K:\n    K,W,N,AA,=W1AW(5)[8],K6(3)[6];\n"
            "*Test Entity:             14:  27:  EU:   50.00:     0.00:     0.0:  *TT:\n    TT;\n";
        DxccDb u;
        std::string err;
        CHECK(u.loadCty(cty, err) && err.empty() && u.entities().size() == 3);
        CHECK(u.lookup("1A0KM").e && u.lookup("1A0KM").e->name == "Sov Mil Order of Malta" && u.lookup("1A0KM").e->lon > 12);
        CHECK(u.lookup("W1AW").prefix == "=W1AW" && u.lookup("K6ABC").cqz == 3 && u.lookup("K6ABC").ituz == 6);
        CHECK(u.lookup("TT1A").e && u.lookup("TT1A").e->primary == "TT" && u.lookup("TT1A").e->adif == 0);
        DxccDb broken;
        CHECK(!broken.loadCty("Name: 1: 2: EU: 0: 0: 0: XX:\n XX", err) && !err.empty());
        CHECK(!broken.loadCty("", err));
    }

    // ---- the award matrix ----
    std::vector<LogQso> log = {
        q("K1ABC", "20m", "FT8", 291, "MA", "FN42", 5, true),
        q("W6XYZ", "20m", "CW", 291, "CA", "CM87", 3, false),
        q("K7AA", "40m", "FT8", 291, "", "DN31", 3, false),          // no state: does not count for WAS
        q("KH6AA", "20m", "SSB", 110, "", "BL11", 31, true),         // Hawaii counts as HI
        q("JA1XYZ", "40m", "FT8", 339, "", "PM95", 25, false),
        q("JA2XYZ", "40m", "FT8", 339, "", "PM84", 25, true),
        q("LY2AA", "80m", "FT8", 146, "", "KO25", 15, false),
        q("DL1AA", "20m", "USB", 0, "", "", 14, false),              // no number, no country: not a DXCC
    };
    log[7].country = "Germany";                                        // now the name counts
    AwardTable dx = awardTable(log, Award::Dxcc);
    CHECK(dx.worked == 5 && dx.confirmed == 3 && dx.total == 340);
    CHECK(dx.bands.size() == 3 && dx.bands[0] == "80m" && dx.bands[1] == "40m" && dx.bands[2] == "20m");
    const AwardRow* ja = nullptr; const AwardRow* us = nullptr;
    for (const auto& r : dx.rows) { if (r.key == "339") ja = &r; if (r.key == "291") us = &r; }
    CHECK(ja && ja->any == 'C' && ja->byBand.at("40m") == 'C' && !ja->byBand.count("20m") && ja->byClass.at("DIGI") == 'C');
    CHECK(us && us->any == 'C' && us->byBand.at("20m") == 'C' && us->byBand.at("40m") == 'W' && us->byClass.at("CW") == 'W' && us->byClass.at("DIGI") == 'C');
    CHECK(dx.perBand.at("20m").first == 3 && dx.perBand.at("20m").second == 2 && dx.perBand.at("40m").first == 2);
    AwardTable dxCw = awardTable(log, Award::Dxcc, "CW");
    CHECK(dxCw.worked == 1 && dxCw.confirmed == 0);
    AwardTable dx40 = awardTable(log, Award::Dxcc, "", "40m");
    CHECK(dx40.worked == 2 && dx40.confirmed == 1);
    AwardTable was = awardTable(log, Award::Was);
    CHECK(was.worked == 3 && was.confirmed == 2 && was.total == 50);
    AwardTable gr = awardTable(log, Award::Grid);
    CHECK(gr.worked == 7 && gr.confirmed == 3);
    AwardTable cz = awardTable(log, Award::Cqz);
    CHECK(cz.worked == 6 && cz.rows.front().key == "3" && cz.rows.back().key == "31");
    CHECK(usStates().size() == 50 && awardKey(log[0], Award::Grid) == "FN42" && awardKey(log[4], Award::Was).empty());

    // ---- statistics ----
    log[0].startUtc = 1000; log[1].startUtc = 2000; log[2].startUtc = 5000;
    const LogStats s = logStats(log, "KO24");
    CHECK(s.qsos == 8 && s.calls == 8 && s.dxcc == 5 && s.grids == 7 && s.confirmed == 3);
    CHECK(s.perBand.at("20m") == 4 && s.perMode.at("FT8") == 5 && s.bestKm > 10000 && !s.bestCall.empty());
    CHECK(s.firstUtc == 1000 && s.lastUtc == 1792672496.0);

    // ---- worked before for the decode list ----
    WorkedIndex wi;
    wi.build(log);
    DecodeMarks m = wi.mark("K1ABC", "FN42", "20m", "FT8", "291");
    CHECK(m.workedCall && m.workedCallBand && !m.newDxcc && !m.newDxccBand && !m.newGrid);
    m = wi.mark("K1ABC/P", "FN42", "15m", "FT8", "291");
    CHECK(m.workedCall && !m.workedCallBand && !m.newDxcc && m.newDxccBand && m.newGridBand && !m.newGrid);
    m = wi.mark("VK2ABC", "QF56", "20m", "FT8", "150");
    CHECK(!m.workedCall && m.newDxcc && m.newGrid);
    m = wi.mark("VK2ABC", "", "20m", "FT8", "");
    CHECK(!m.newDxcc && !m.newGrid);                                   // unknown entity, no grid: no marks
    LogQso nq = q("VK2ABC", "20m", "FT8", 150, "", "QF56", 30, false);
    wi.add(nq);
    CHECK(wi.mark("VK3AA", "QF22", "20m", "FT8", "150").newDxcc == false);

    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ham awards: all passed\n");
    return 0;
}
