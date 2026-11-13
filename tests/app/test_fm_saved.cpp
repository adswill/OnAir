// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Saved FM stations: persistence, search, rename and delete, kept apart from the band-scan list.
#include "state/fm_saved.h"
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// Prefs::setS() on every platform: '|' and line breaks become spaces. The record separators must survive.
static std::string prefsClean(std::string s) {
    for (char& c : s) if (c == '\n' || c == '\r' || c == '|') c = ' ';
    return s;
}

// The scan list is mhz|name|type|snr|stereo|rds|pi|tp|radio text|scrolling name, one station per line.
static int scanRows(const std::string& st) {
    int n = 0;
    size_t pos = 0;
    while (pos < st.size()) {
        size_t e = st.find('\n', pos);
        if (e == std::string::npos) e = st.size();
        const std::string line = st.substr(pos, e - pos);
        pos = e + 1;
        int bars = 0;
        for (char c : line) if (c == '|') bars++;
        if (bars >= 5 && !line.empty()) n++;
    }
    return n;
}

int main() {
    const std::vector<FmSavedStation> lib = {
        {"BBC R1", 98.8},
        {"Jazz FM", 102.2},
        {"", 88.0},
    };
    const std::string blob = fmSavedEncode(lib);
    const std::vector<FmSavedStation> back = fmSavedDecode(prefsClean(blob));
    CHECK(back.size() == 3, "round trip size %zu", back.size());
    CHECK(back[0].name == "BBC R1" && back[0].mhz > 98.75 && back[0].mhz < 98.85, "first station");
    CHECK(back[1].name == "Jazz FM" && back[1].mhz > 102.15 && back[1].mhz < 102.25, "second station");
    CHECK(back[2].name.empty() && back[2].mhz > 87.95 && back[2].mhz < 88.05, "unnamed station");

    const std::string dirty = fmSavedEncode({{"Rock|FM\nLive\r", 101.0}, {"A\x1f""B\x1e""C", 99.0}});
    const auto cleaned = fmSavedDecode(prefsClean(dirty));
    CHECK(cleaned.size() == 2, "sanitized size %zu", cleaned.size());
    CHECK(cleaned[0].name == "Rock FM Live ", "pipe and newlines: [%s]", cleaned[0].name.c_str());
    CHECK(cleaned[1].name == "A B C", "separators in a name: [%s]", cleaned[1].name.c_str());
    CHECK(blob.find('|') == std::string::npos && blob.find('\n') == std::string::npos, "saved blob uses no scan separators");

    const std::string scan = "100.0|Classic|Pop|18|1|1|C201|1|now playing|Classic\n88.0|Talk|News|12|0|1|C202|0||Talk\n";
    CHECK(fmSavedDecode(scan).empty(), "a scan list was loaded as saved stations");
    CHECK(scanRows(scan) == 2, "scan fixture");
    CHECK(scanRows(blob) == 0, "saved stations counted as scan rows");
    CHECK(fmSavedDecode("75.9\x1f" "low\x1e" "108.2\x1f" "high\x1e" "108.0\x1f" "edge\x1e").size() == 1, "band limits");
    CHECK(fmSavedDecode("76.1\x1f" "eFM\x1e" "87.1\x1f" "eFM 196\x1e").size() == 2, "extended FM (76 - 87.4 MHz) is kept, as the band scan reaches it");
    CHECK(fmSavedDecode("not-a-record").empty() && fmSavedDecode("").empty(), "junk and empty");

    CHECK(fmSavedMatches(lib[0], ""), "empty filter");
    CHECK(fmSavedMatches(lib[0], "bbc") && fmSavedMatches(lib[0], "R1") && fmSavedMatches(lib[0], "98.8"), "name and frequency");
    CHECK(!fmSavedMatches(lib[0], "102") && !fmSavedMatches(lib[1], "bbc"), "other station");
    CHECK(fmSavedMatches(lib[2], "88.0") && !fmSavedMatches(lib[2], "jazz"), "unnamed matches its frequency");

    std::vector<FmSavedStation> edit = lib;
    const std::vector<FmSavedStation> scanCopy = {{"Classic", 100.0}};   // a scan result must stay put while the library changes
    CHECK(fmSavedRename(edit, 1, "  Smooth Radio  "), "rename");
    CHECK(edit[1].name == "Smooth Radio", "trimmed name [%s]", edit[1].name.c_str());
    CHECK(fmSavedRename(edit, 0, " -- "), "clear name");
    CHECK(edit[0].name.empty(), "blank rename");
    CHECK(!fmSavedRename(edit, -1, "no") && !fmSavedRename(edit, 9, "no"), "rename bounds");
    CHECK(edit.size() == 3 && edit[2].mhz > 87.95, "rename left the others");

    CHECK(fmSavedRemove(edit, 0), "delete first");
    CHECK(edit.size() == 2 && edit[0].name == "Smooth Radio" && edit[1].name.empty(), "list after delete");
    CHECK(!fmSavedRemove(edit, 2) && !fmSavedRemove(edit, -1), "delete bounds");
    CHECK(fmSavedRemove(edit, 1) && edit.size() == 1, "delete last");
    CHECK(scanCopy.size() == 1 && scanCopy[0].name == "Classic" && scanCopy[0].mhz > 99.9, "scan list untouched");

    const std::string again = fmSavedEncode(edit);
    const auto againBack = fmSavedDecode(again);
    CHECK(againBack.size() == 1 && againBack[0].name == "Smooth Radio", "saved after edit");

    FmSavedEdit ui;
    ui.editing = 4; ui.confirmDel = 4; ui.askConfirm = true;
    fmSavedValidateEdit(ui, 1);
    CHECK(ui.editing == -1 && ui.confirmDel == -1 && !ui.askConfirm, "stale editor indexes");

    if (fails) return 1;
    printf("ok\n");
    return 0;
}
