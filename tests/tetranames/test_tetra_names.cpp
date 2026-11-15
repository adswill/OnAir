// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA name book (core/src/tetra/tetra_names.cpp): own CSV round trip, tolerant line formats, network-specific lookup, edit, filter,
// damaged rows, persistence through a file. All ids and names are made up.
#include "dect2/tetra/tetra_names.h"
#include <cstdio>
#include <filesystem>
using namespace dect2;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    size_t sk = 0;
    // tolerant line formats
    {
        TetraNames d;
        const std::string t = "\xEF\xBB\xBF# made-up list\r\n"
                              "1001;Alpha Unit\r\n"
                              "1002,\"Bravo, Unit\",spare radio\r\n"
                              "1003\tCharlie Unit\r\n"
                              "1004 Delta Unit\r\n"
                              "1005=Echo Unit\r\n"
                              "GSSI 2001 Test Group A\r\n"
                              "ISSI:1006;Foxtrot Unit\r\n"
                              "GSSI:2002;Test Group B;notes here\r\n"
                              "\r\n"
                              "abc;not an id\r\n"
                              "0;zero id\r\n"
                              "99999999;too big\r\n"
                              "1007;\r\n";
        CHECK(d.import(t, TetraIdKind::Issi, &sk) == 8 && sk == 4);
        CHECK(d.label(TetraIdKind::Issi, 1001) == "Alpha Unit");
        CHECK(d.label(TetraIdKind::Issi, 1002) == "Bravo, Unit");
        TetraNameEntry e; CHECK(d.find(TetraIdKind::Issi, 1002, -1, -1, e) && e.notes == "spare radio");
        CHECK(d.label(TetraIdKind::Issi, 1003) == "Charlie Unit");
        CHECK(d.label(TetraIdKind::Issi, 1004) == "Delta Unit");
        CHECK(d.label(TetraIdKind::Issi, 1005) == "Echo Unit");
        CHECK(d.label(TetraIdKind::Issi, 1006) == "Foxtrot Unit");
        CHECK(d.label(TetraIdKind::Gssi, 2001) == "Test Group A");
        CHECK(d.label(TetraIdKind::Issi, 2001).empty());   // kinds are separate
        CHECK(d.find(TetraIdKind::Gssi, 2002, -1, -1, e) && e.notes == "notes here");
        TetraNames g; CHECK(g.import("3001;Group X\n3002=Group Y\n", TetraIdKind::Gssi) == 2 && g.label(TetraIdKind::Gssi, 3002) == "Group Y");   // default kind
    }
    // own CSV with header: round trip, quotes, networks
    {
        TetraNames t;
        t.set({TetraIdKind::Issi, 5001, "Name, with \"quotes\"", "note", "", 901, 1, 3});
        t.set({TetraIdKind::Issi, 5001, "Other Network Name", "", "", 262, 10, -1});
        t.set({TetraIdKind::Issi, 5002, "Anywhere", "", "Site 7", -1, -1, -1});
        t.set({TetraIdKind::Gssi, 6001, "Group", "", "", -1, -1, -1});
        const std::string out = t.exportCsv();
        TetraNames u; CHECK(u.import(out, TetraIdKind::Gssi, &sk) == 4 && sk == 0);
        CHECK(u.label(TetraIdKind::Issi, 5001, 901, 1) == "Name, with \"quotes\"");
        CHECK(u.label(TetraIdKind::Issi, 5001, 262, 10) == "Other Network Name");
        CHECK(u.label(TetraIdKind::Issi, 5001, 100, 1).empty());          // a known other network: no match
        CHECK(!u.label(TetraIdKind::Issi, 5001).empty());                 // network unknown: any entry
        CHECK(u.label(TetraIdKind::Issi, 5002, 901, 1) == "Anywhere");     // any-network entry applies everywhere
        TetraNameEntry e; CHECK(u.find(TetraIdKind::Issi, 5001, 901, 1, e) && e.colourCode == 3 && e.mcc == 901 && e.mnc == 1);
        CHECK(u.find(TetraIdKind::Issi, 5002, -1, -1, e) && e.site == "Site 7");
        CHECK(u.exportCsv() == out);
        CHECK(u.exportCsv(false, TetraIdKind::Gssi).find("5001") == std::string::npos && u.exportCsv(false, TetraIdKind::Gssi).find("6001") != std::string::npos);
        // semicolon header and odd order, type column missing
        TetraNames v; CHECK(v.import("name;GSSI;mcc;mnc\nNorth;7001;901;1\nBad;7002;901;\nSouth;7003;;\n", TetraIdKind::Issi, &sk) == 2 && sk == 1);
        CHECK(v.label(TetraIdKind::Issi, 7001, 901, 1) == "North" && v.label(TetraIdKind::Issi, 7003, 5, 5) == "South");
    }
    // edit, filter, erase
    {
        TetraNames d;
        d.set({TetraIdKind::Issi, 10, "Alpha", "", "North depot", -1, -1, -1});
        d.set({TetraIdKind::Issi, 20, "Bravo", "spare", "", -1, -1, -1});
        d.set({TetraIdKind::Issi, 10, "Alpha 2", "", "", -1, -1, -1});   // replace
        CHECK(d.count(TetraIdKind::Issi) == 2 && d.label(TetraIdKind::Issi, 10) == "Alpha 2");
        CHECK(d.entries(TetraIdKind::Issi, "spare").size() == 1 && d.entries(TetraIdKind::Issi, "BRAVO spare").size() == 1 && d.entries(TetraIdKind::Issi, "zzz").empty());
        CHECK(d.entries(TetraIdKind::Issi, "")[0].id == 10);
        d.set({TetraIdKind::Issi, 20, "", "", "", -1, -1, -1});           // empty name removes
        CHECK(d.size() == 1 && !d.erase(TetraIdKind::Issi, 20) && d.erase(TetraIdKind::Issi, 10) && d.size() == 0);
    }
    // global book + file persistence
    {
        const std::string path = (std::filesystem::temp_directory_path() / "onair_tetra_names_test" / "tetra_names.csv").string();
        std::filesystem::remove_all(std::filesystem::path(path).parent_path());
        const uint64_t r0 = tetraNamesRevision();
        tetraNamesEdit([](TetraNames& d) { d.set({TetraIdKind::Issi, 42, "Persisted", "n", "s", 901, 1, 2}); d.set({TetraIdKind::Gssi, 43, "Group P", "", "", -1, -1, -1}); });
        CHECK(tetraNamesRevision() > r0);
        CHECK(tetraNameLabel(TetraIdKind::Issi, 42, 901, 1) == "Persisted" && tetraNameWithId(TetraIdKind::Issi, 42, 901, 1) == "42  Persisted" && tetraNameWithId(TetraIdKind::Issi, 99) == "99");
        CHECK(tetraNamesSaveFile(path, tetraNamesSnapshot()));   // creates the folder
        TetraNames back; CHECK(tetraNamesLoadFile(path, back, &sk) && sk == 0 && back.size() == tetraNamesSnapshot().size());
        TetraNameEntry e; CHECK(back.find(TetraIdKind::Issi, 42, 901, 1, e) && e.name == "Persisted" && e.notes == "n" && e.site == "s" && e.colourCode == 2);
        CHECK(back.label(TetraIdKind::Gssi, 43) == "Group P");
        CHECK(!tetraNamesLoadFile(path + ".missing", back));
        std::filesystem::remove_all(std::filesystem::path(path).parent_path());
    }
    if (fails == 0) printf("tetra names: all passed\n");
    return fails ? 1 : 0;
}
