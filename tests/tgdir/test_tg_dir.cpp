// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Talkgroup directory (core/src/common/tg_dir.cpp): own CSV round trip, RadioReference-style import, headerless, separators, quotes, per-kind
// separation, edit, filter, damaged rows.
#include "dect2/common/tg_dir.h"
#include <cstdio>
using namespace dect2;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    TgDir d; size_t sk = 0;
    // RadioReference talkgroup export: Decimal,Hex,Alpha Tag,Mode,Description,Tag,Category
    const std::string rr = "Decimal,Hex,Alpha Tag,Mode,Description,Tag,Category\r\n"
                           "1001,3E9,\"PD Dispatch\",D,\"Police, main dispatch\",Law Dispatch,Police\r\n"
                           "1002,3EA,PD Tac 1,DE,Tactical,Law Tac,Police\r\n"
                           "x,3EB,Bad,D,bad id,Law,Police\r\n"
                           ",,,,,,\r\n"
                           "1003,3EB,,D,Fire Dispatch only description,Fire Dispatch,Fire\r\n";
    CHECK(d.import(rr, TgKind::P25, &sk) == 3 && sk == 1);
    CHECK(d.label(TgKind::P25, 1001) == "PD Dispatch" && d.label(TgKind::Dmr, 1001).empty());   // the kind of the panel only
    TgDirEntry e; CHECK(d.find(TgKind::P25, 1001, e) && e.group == "Law Dispatch" && e.note == "Police, main dispatch");
    CHECK(d.label(TgKind::P25, 1003) == "Fire Dispatch only description");   // no Alpha Tag: the description
    // own format round trip with all kinds, quotes and commas in names
    TgDir t;
    t.set({TgKind::Dmr, 91, "World-wide, \"WW\"", "BrandMeister", "global"});
    t.set({TgKind::Tetra, 91, "Dispatch 1", "", ""});
    t.set({TgKind::Dmr, 262, "Germany", "", ""});
    const std::string out = t.exportCsv(true);
    TgDir u; CHECK(u.import(out, TgKind::P25, &sk) == 3 && sk == 0);
    CHECK(u.label(TgKind::Dmr, 91) == "World-wide, \"WW\"" && u.label(TgKind::Tetra, 91) == "Dispatch 1" && u.label(TgKind::Dmr, 262) == "Germany");
    CHECK(u.count(TgKind::Dmr) == 2 && u.count(TgKind::P25) == 0);
    // one kind exported: re-imported as a panel's list
    TgDir v; CHECK(v.import(t.exportCsv(false, TgKind::Dmr), TgKind::Dmr) == 2 && v.size() == 2);
    // headerless, semicolons, tabs, BOM, "any" kind
    TgDir w;
    CHECK(w.import("\xEF\xBB\xBF" "10;Alpha;grp\n20;Bravo\n", TgKind::Tetra) == 2 && w.label(TgKind::Tetra, 20) == "Bravo");
    CHECK(w.import("30\tCharlie\n", TgKind::Dmr) == 1 && w.label(TgKind::Dmr, 30) == "Charlie");
    CHECK(w.import("Kind,Id,Name\nany,40,Delta\nZZZ,41,Bad\n", TgKind::P25, &sk) == 1 && sk == 1 && w.label(TgKind::P25, 40) == "Delta");
    // header without a name or id column: nothing, counted
    TgDir z; CHECK(z.import("foo,bar\n1,2\n", TgKind::Dmr, &sk) == 0 && sk == 1);
    // edit, erase, replace, empty name removes
    t.set({TgKind::Dmr, 91, "WW", "", ""}); CHECK(t.label(TgKind::Dmr, 91) == "WW" && t.size() == 3);
    t.set({TgKind::Dmr, 91, "  ", "", ""}); CHECK(t.label(TgKind::Dmr, 91).empty() && t.size() == 2);
    CHECK(t.erase(TgKind::Tetra, 91) && !t.erase(TgKind::Tetra, 91));
    // filter: id, name, group, words
    CHECK(d.entries(TgKind::P25, "").size() == 3);
    CHECK(d.entries(TgKind::P25, "police dispatch").size() == 1 && d.entries(TgKind::P25, "1002").size() == 1 && d.entries(TgKind::P25, "fire").size() == 1);
    CHECK(d.entries(TgKind::P25, "nothere").empty() && d.entries(TgKind::Dmr, "").empty());
    // the app-wide directory
    const uint64_t r0 = tgRevision();
    tgEdit([&](TgDir& g) { g.set({TgKind::Dmr, 7, "Seven", "", ""}); });
    CHECK(tgRevision() > r0 && tgLabel(TgKind::Dmr, 7) == "Seven" && tgLabel(TgKind::Tetra, 7).empty() && tgSnapshot().size() == 1);
    printf(fails ? "tg_dir: %d FAILED\n" : "tg_dir: ok\n", fails);
    return fails ? 1 : 0;
}
