// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The frequency database (core/src/channels/freq_db.cpp, data/frequencies/*.tsv): the data is valid (known mode ids, no duplicate per mode and
// frequency, frequencies inside each mode's band, categories from the fixed list, a source on every entry, small), search and filters, the
// conversion to a Memory, and the import / export round trip of the user's entries.
#include "dect2/channels/freq_db.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static FreqEntry mk(const char* name, uint64_t hz, const char* mode) { FreqEntry e; e.name = name; e.hz = hz; e.mode = mode; e.user = true; return e; }

static void testData() {
    const auto& db = builtinFrequencies();
    size_t total = 0;
    for (size_t i = 0; i < freqDataFileCount(); i++) {
        FreqParseReport rep;
        const char* t = freqDataFile(i);
        total += std::strlen(t);
        parseFreqTsv(t, rep);
        CHECK(rep.skipped == 0, "data file %zu has %zu bad lines (first: %s)", i, rep.skipped, rep.log.empty() ? "?" : rep.log[0].c_str());
        CHECK(std::strlen(t) < 16000, "data file %zu is %zu bytes: split it (MSVC string literal limit)", i, std::strlen(t));
    }
    CHECK(total < 400 * 1024, "the data is %zu bytes (limit 400 KB)", total);
    CHECK(db.size() >= 800, "only %zu built-in entries", db.size());

    // the band of each mode that has one (MHz); the modes without a row are only checked against the receiver's overall range
    const std::map<std::string, std::pair<double, double>> band = {
        {"adsb", {1085, 1095}}, {"uplink", {1025, 1035}}, {"uat", {970, 985}}, {"acars", {118, 140}}, {"vdl2", {118, 140}}, {"vdl4", {118, 140}},
        {"airband", {118, 400}}, {"vor", {108, 118}}, {"ils", {108, 112}}, {"ndb", {0.19, 0.54}}, {"ais", {156, 163}}, {"vdes", {150, 165}},
        {"dsc", {0.5, 163}}, {"navtex", {0.4, 4.3}}, {"wefax", {2, 20}}, {"hfdl", {2, 22}}, {"dab", {174, 240}}, {"same", {162.4, 162.55}},
        {"gnss", {1150, 2500}}, {"wifi", {2400, 7125}}, {"ble", {2400, 2484}}, {"zigbee", {863, 2484}}, {"dect", {1880, 1900}}, {"ft8", {1.8, 500}},
        {"wspr", {0.4, 150}}, {"time", {0.03, 30}}, {"hfbeacon", {14, 29}}, {"codar", {3, 50}}, {"sonde", {380, 1700}}, {"iridium", {1610, 1630}},
        {"qo100", {10480, 10500}}, {"dpmr", {446, 447}}, {"dmr", {400, 470}}, {"meteor", {136, 139}}, {"orbcomm", {136, 139}}, {"hrpt", {1690, 1710}},
        {"goes", {1670, 1710}}, {"packet", {10, 440}}, {"loraaprs", {430, 440}}, {"pocsag", {130, 470}}, {"flex", {130, 940}}, {"fm", {64, 110}},
        {"dvb", {170, 800}}, {"atsc", {50, 700}}, {"isdbt", {470, 720}}};
    std::set<std::string> seen;
    std::map<std::string, size_t> perMode;
    size_t bad = 0;
    for (const FreqEntry& e : db) {
        const std::string where = e.mode + " " + e.name;
        if (!freqModeIdValid(e.mode)) { printf("FAIL: unknown mode id in '%s'\n", where.c_str()); fails++; bad++; }
        const double mhz = (double)e.hz / 1e6;
        if (mhz < 0.01 || mhz > 12000) { printf("FAIL: '%s' %.6f MHz is out of range\n", where.c_str(), mhz); fails++; }
        auto b = band.find(e.mode);
        const bool qoRf = e.name.rfind("QO-100", 0) == 0 && mhz > 10480;   // the QO-100 presets list the RF downlink; the receiver tunes the IF behind the LNB
        if (b != band.end() && !qoRf && (mhz < b->second.first || mhz > b->second.second)) { printf("FAIL: '%s' %.6f MHz is outside the %s band\n", where.c_str(), mhz, e.mode.c_str()); fails++; }
        if (!seen.insert(freqKey(e)).second) { printf("FAIL: duplicate %s %.6f MHz (%s)\n", e.mode.c_str(), mhz, e.name.c_str()); fails++; }
        bool cat = false;
        for (const std::string& c : freqCategories()) if (c == e.category) cat = true;
        if (!cat) { printf("FAIL: '%s' has category '%s'\n", where.c_str(), e.category.c_str()); fails++; }
        if (e.source.empty()) { printf("FAIL: '%s' has no source\n", where.c_str()); fails++; }
        if (e.region.empty()) { printf("FAIL: '%s' has no region\n", where.c_str()); fails++; }
        if (e.user) { printf("FAIL: '%s' built in but marked user\n", where.c_str()); fails++; }
        perMode[e.mode]++;
    }
    (void)bad;
    // the modes the database promises to cover
    for (const char* m : {"adsb", "uat", "acars", "vdl2", "hfdl", "ais", "dsc", "navtex", "wefax", "same", "pocsag", "flex", "packet", "dstar", "dmr", "dpmr", "p25", "tetra",
                          "time", "hfbeacon", "codar", "iridium", "orbcomm", "inmc", "globalstar", "gnss", "ism", "lorawan", "meshtastic", "wifi", "ble", "dji", "rid", "sonde",
                          "dab", "dvb", "atsc", "fm", "airband", "cubesat", "qo100", "ft8", "wspr", "psk", "js8", "sstv", "cw", "general", "meteor", "hrpt", "goes"})
        CHECK(perMode[m] > 0, "no entries for mode %s", m);
    // QO-100: a "QO-100 NB" preset for every mode used on the narrowband transponder, inside the AMSAT-DL plan, and the DATV ones for DVB-S/S2
    for (const char* m : {"cw", "ft8", "ft4", "js8", "psk", "rtty", "wspr", "sstv", "freedv", "hamdrm", "packet", "olivia", "contestia", "mfsk", "thor", "dominoex", "hell", "mt63", "general"}) {
        bool have = false;
        for (const FreqEntry& e : db) if (e.mode == m && e.name.rfind("QO-100 NB", 0) == 0) { have = true; CHECK(e.hz >= 10489505000ull && e.hz <= 10489990000ull, "QO-100 NB %s preset %.3f MHz is inside the narrowband plan", m, (double)e.hz / 1e6); }
        CHECK(have, "no QO-100 NB preset for %s", m);
    }
    { bool beacon = false, wide = false; for (const FreqEntry& e : db) if (e.mode == "dvbs") { beacon |= e.hz == 10491500000ull; wide |= e.hz == 10495000000ull; } CHECK(beacon && wide, "QO-100 DATV presets for DVB-S/S2"); }
    printf("freq db: %zu entries, %zu modes, %zu bytes in %zu files\n", db.size(), perMode.size(), total, freqDataFileCount());
}

static void testSearch() {
    const auto& db = builtinFrequencies();
    FreqQuery q;
    q.text = "162.4";
    auto r = freqSearch(db, q);
    CHECK(!r.empty(), "162.4 finds the weather radio");
    bool wx = false;
    for (size_t i : r) wx |= db[i].mode == "same" && db[i].hz == 162400000;
    CHECK(wx, "162.4 finds NOAA WX1");
    for (size_t k = 1; k < r.size(); k++) CHECK(db[r[k - 1]].hz <= db[r[k]].hz, "results ordered by frequency");

    q = FreqQuery();
    q.mode = "adsb";
    r = freqSearch(db, q);
    CHECK(r.size() == 1 && db[r[0]].hz == 1090000000, "adsb has 1090 MHz");
    CHECK(freqForMode(db, "adsb").size() == 1, "freqForMode");
    CHECK(freqForMode(db, "no-such-mode").empty(), "unknown mode has none");

    q = FreqQuery();
    q.text = "ncdxf beacon";
    r = freqSearch(db, q);
    CHECK(r.size() == 5, "five NCDXF beacon channels (got %zu)", r.size());

    q = FreqQuery();
    q.region = "EU";
    q.mode = "lorawan";
    r = freqSearch(db, q);
    bool us = false, eu = false;
    for (size_t i : r) { us |= db[i].region == "US"; eu |= db[i].region == "EU"; }
    CHECK(eu && !us, "region filter keeps EU, drops US");
    q.region = "US";
    r = freqSearch(db, q);
    bool anyUs = false;
    for (size_t i : r) anyUs |= db[i].region == "US";
    CHECK(anyUs, "US lorawan entries");

    q = FreqQuery();
    q.category = "time";
    r = freqSearch(db, q);
    CHECK(!r.empty(), "time category");
    for (size_t i : r) CHECK(db[i].category == "time", "category filter exact");

    q = FreqQuery();
    q.text = "wwv";
    CHECK(freqSearch(db, q).size() >= 5, "wwv search");
    q.text = "WWV 10";   // words are ANDed and case-insensitive
    r = freqSearch(db, q);
    CHECK(!r.empty() && r.size() < 6, "two words narrow the search (%zu)", r.size());
    q.text = "zzzz-nothing";
    CHECK(freqSearch(db, q).empty(), "no match");

    // favourites
    std::set<std::string> fav = {freqKey(db[0])};
    q = FreqQuery();
    q.favouritesOnly = true;
    r = freqSearch(db, q, &fav);
    CHECK(r.size() == 1 && r[0] == 0, "favourites only");
    CHECK(freqSearch(db, q, nullptr).empty(), "favourites only without a set is empty");

    CHECK(freqDistinct(db, 0).size() > 40, "distinct modes");
    CHECK(!freqDistinct(db, 1).empty() && !freqDistinct(db, 2).empty(), "distinct regions and categories");
    CHECK(freqModeName("ft8") == "FT8" && freqModeName("adsb") == "ADS-B" && freqModeName("fm") == "FM" && freqModeName("zzz") == "zzz", "mode names");
    CHECK(freqModeIdValid("meshtastic") && freqModeIdValid("fm") && freqModeIdValid("ft8") && !freqModeIdValid("mesh") && !freqModeIdValid("pager") && !freqModeIdValid("nonsense"), "mode id validity");
}

static void testParser() {
    FreqParseReport rep;
    const std::string t = "# c\n@mode ft8\n@region EU\n@category amateur\n@source test\n14.074|FT8 20 m|3|usb\n\n"
                          "7.074|FT8 40 m||| psk | US | beacon\nbad|x\n14.1||\n0|zero\n3.5|name|-1\n@weird x\n5|ok\n";
    const auto v = parseFreqTsv(t, rep);
    CHECK(v.size() == 3, "parser keeps %zu entries", v.size());
    CHECK(rep.skipped == 5, "parser skipped %zu", rep.skipped);
    if (v.size() == 3) {
        CHECK(v[0].hz == 14074000 && v[0].mode == "ft8" && v[0].region == "EU" && v[0].bwHz == 3000 && v[0].notes == "usb" && v[0].source == "test", "first line");
        CHECK(v[1].mode == "psk" && v[1].region == "US" && v[1].category == "beacon", "per line overrides");
    }
}

static void testIo() {
    std::vector<FreqEntry> v;
    FreqEntry a = mk("Caf\xC3\xA9 \"Radio\", 93,9\nline2", 93900000, "fm");
    a.region = "AE"; a.category = "my list"; a.notes = "tab\there, \"q\""; a.bwHz = 180000;
    v.push_back(a);
    v.push_back(mk("Plain", 14074000, "ft8"));
    v.push_back(mk("Odd Hz", 162412500, "same"));

    for (int csv = 0; csv < 2; csv++) {
        const std::string text = csv ? freqExportCsv(v) : freqExportJson(v);
        std::vector<FreqEntry> back;
        FreqParseReport rep = freqImportText(text, back);
        CHECK(rep.loaded == 3 && back.size() == 3, "%s import loaded %zu", csv ? "csv" : "json", rep.loaded);
        if (back.size() != 3) continue;
        for (size_t i = 0; i < 3; i++) {
            CHECK(back[i].hz == v[i].hz && back[i].mode == v[i].mode && back[i].name == v[i].name, "%s entry %zu", csv ? "csv" : "json", i);
            CHECK(back[i].user, "imported entries are the user's");
        }
        CHECK(back[0].region == "AE" && back[0].category == "my list" && back[0].notes == a.notes && back[0].bwHz == 180000, "%s fields of entry 0", csv ? "csv" : "json");
        // importing the same file again adds nothing
        rep = freqImportText(text, back);
        CHECK(rep.loaded == 0 && back.size() == 3, "second import is all duplicates (%zu)", rep.loaded);
    }
    // a plain "frequency,name" list
    std::vector<FreqEntry> plain;
    FreqParseReport rep = freqImportText("162.4,Weather\n146.52,Calling\n", plain);
    CHECK(rep.loaded == 2 && plain.size() == 2 && plain[0].hz == 162400000, "plain list import");
    std::vector<FreqEntry> junk;
    rep = freqImportText("this is not a list\n\x01\x02", junk);
    CHECK(junk.empty(), "garbage imports nothing");

    // to a Memory and back
    const Memory m = freqToMemory(a);
    CHECK(m.freqHz == a.hz && m.mode == "fm" && m.name == a.name && m.bandwidthHz == 180000 && m.group == "AE", "freqToMemory");
    const FreqEntry b = freqFromMemory(m);
    CHECK(b.hz == a.hz && b.mode == a.mode && b.category == a.category, "freqFromMemory");
}

// Issue #411: the rows of a search can be exported as CSV and read back
static void testExportResults() {
    std::vector<FreqEntry> all = {mk("VDES ASM 1", 161950000, "vdes"), mk("VDES ASM 2", 162000000, "vdes"), mk("Tower, \"north\"", 118100000, "airband"), mk("Noor", 93900000, "fm")};
    FreqQuery q; q.text = "asm";
    std::vector<FreqEntry> rows;
    for (size_t i : freqSearch(all, q)) rows.push_back(all[i]);
    CHECK(rows.size() == 2, "search asm finds two rows");
    const std::string csv = freqExportCsv(rows);
    CHECK(csv.find("VDES ASM 1") != std::string::npos && csv.find("VDES ASM 2") != std::string::npos && csv.find("Noor") == std::string::npos && csv.find("Tower") == std::string::npos, "only the searched rows are in the CSV");
    std::vector<FreqEntry> back;
    freqImportText(csv, back);
    CHECK(back.size() == 2 && back[0].hz + back[1].hz == 161950000ull + 162000000ull, "the exported results import again");
}

// Issue #411: ASM 1 and ASM 2 are channels 2027 and 2028 of Radio Regulations Appendix 18
static void testVdesAsmChannels() {
    int found = 0;
    for (const FreqEntry& e : builtinFrequencies()) {
        if (e.name == "VDES ASM 1") { CHECK(e.hz == 161950000ull, "ASM 1 is 161.950 MHz"); found++; }
        if (e.name == "VDES ASM 2") { CHECK(e.hz == 162000000ull, "ASM 2 is 162.000 MHz"); found++; }
    }
    CHECK(found == 2, "both ASM channels are listed");
}

int main() {
    testExportResults();
    testVdesAsmChannels();
    testData();
    testSearch();
    testParser();
    testIo();
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("freq_db OK\n");
    return 0;
}
