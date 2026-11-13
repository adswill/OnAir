// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The airband channel list in the settings file (#148): add, edit, delete and special characters survive a save and a load, through the same
// "key=value" line the settings file keeps, and the older forms still load.
#include "dect2/airband/airband_store.h"
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

// what FilePrefs does with one value: written on one line, read back up to the line end
static std::string viaFile(const std::string& v) {
    std::string clean = v;
    for (char& c : clean) if (c == '\n' || c == '\r' || c == '|') c = ' ';   // FilePrefs::setS
    std::stringstream f;
    f << "other=1\nairChans=" << clean << "\nairSquelch=6\n";
    std::string line, got;
    bool found = false;
    while (std::getline(f, line)) { const size_t eq = line.find('='); if (eq != std::string::npos && line.substr(0, eq) == "airChans") { got = line.substr(eq + 1); found = true; } }
    return found ? got : "?";
}
static AirbandChannel ch(double f, bool n, const char* label, bool m = false, bool s = false, bool p = false) {
    AirbandChannel c; c.freqHz = f; c.is833 = n; c.label = label; c.muted = m; c.solo = s; c.priority = p; return c;
}
static bool same(const std::vector<AirbandChannel>& a, const std::vector<AirbandChannel>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::abs(a[i].freqHz - b[i].freqHz) > 0.5 || a[i].is833 != b[i].is833 || a[i].label != b[i].label || a[i].muted != b[i].muted || a[i].solo != b[i].solo || a[i].priority != b[i].priority || a[i].squelchDb != b[i].squelchDb) return false;
    return true;
}

int main() {
    std::vector<AirbandChannel> v = {ch(118400000, false, "test"), ch(119000000, true, "test2", true), ch(119016666.7, true, "test3", false, true, true)};
    CHECK(same(airbandDecode(viaFile(airbandEncode(v))), v), "the three channels of the report");

    v.push_back(ch(121500000, false, "Guard"));   // add
    CHECK(same(airbandDecode(viaFile(airbandEncode(v))), v), "after an add");
    v[1].label = "Tower 2";                       // edit
    CHECK(same(airbandDecode(viaFile(airbandEncode(v))), v), "after an edit");
    v.erase(v.begin());                           // delete
    CHECK(same(airbandDecode(viaFile(airbandEncode(v))), v), "after a delete");
    while (!v.empty()) v.pop_back();              // delete the last one: an empty list is saved as empty, not as "never saved"
    CHECK(airbandDecode(viaFile(airbandEncode(v))).empty(), "an empty list stays empty");

    // labels with every character that broke a line, a field or a record
    const char* odd[] = {"a,b;c", "x=y", "50% off %41", "line\nbreak", "cr\rhere", "p|q", "tab\there", "\x1e\x1f", "Zürich Tower \xC3\xA9", "", "  spaced  ", "%", "%4", "\x01\x7f"};
    std::vector<AirbandChannel> w;
    double f = 118000000;
    for (const char* l : odd) { w.push_back(ch(f, false, l)); f += 25000; }
    const auto back = airbandDecode(viaFile(airbandEncode(w)));
    CHECK(back.size() == w.size(), "special characters: %zu of %zu channels came back", back.size(), w.size());
    for (size_t i = 0; i < back.size() && i < w.size(); i++) CHECK(back[i].label == w[i].label, "label %zu: \"%s\" came back as \"%s\"", i, w[i].label.c_str(), back[i].label.c_str());
    CHECK(airbandEncode(w).find_first_of("\n\r|\x1e\x1f") == std::string::npos, "the text has no control character or separator of the file");

    // older saves still load: 0.2.5 (0x1e / 0x1f) and the '|' form of the macOS preferences
    const std::string v1 = std::string("118400000.0\x1f" "0\x1f" "0\x1f" "0\x1f" "0\x1f" "test\x1e" "119000000.0\x1f" "1\x1f" "0\x1f" "0\x1f" "0\x1f" "test2\x1e");
    const auto o1 = airbandDecode(v1);
    CHECK(o1.size() == 2 && o1[1].label == "test2" && o1[1].is833, "the 0.2.5 form: %zu channels", o1.size());
    const auto o2 = airbandDecode("118400000.0|0|0|0|0|test;119000000.0|1|0|1|0|test2;");
    CHECK(o2.size() == 2 && o2[1].solo && o2[0].label == "test", "the '|' form: %zu channels", o2.size());

    // a damaged or off-raster record is skipped, the others are kept
    const auto d = airbandDecode("v2:118400000.0,0,0,0,0,ok;garbage;118400100.0,0,0,0,0,off raster;119000000.0,1,0,0,0,fine;");
    CHECK(d.size() == 2 && d[0].label == "ok" && d[1].label == "fine", "damaged records: %zu channels", d.size());
    CHECK(airbandDecode("").empty() && airbandDecode("v2:").empty() && airbandDecode(";;;").empty(), "empty forms");
    // per-channel squelch: saved, and a v2 list (no squelch field) loads with the global one
    {
        std::vector<AirbandChannel> v = {ch(118.5e6, false, "A"), ch(118.7e6, false, "B")};
        v[1].squelchDb = 12.5f;
        const auto back = airbandDecode(viaFile(airbandEncode(v)));
        CHECK(same(v, back) && back.size() == 2 && back[0].squelchDb == kAirbandSquelchGlobal && back[1].squelchDb == 12.5f, "squelch round trip");
        const auto old = airbandDecode("v2:118500000.0,0,0,0,0,Tower;118700000.0,0,0,0,0,;");
        CHECK(old.size() == 2 && old[0].squelchDb == kAirbandSquelchGlobal && old[0].label == "Tower", "v2 list");
        const auto bad = airbandDecode("v3:118500000.0,0,0,0,0,X,999;118700000.0,0,0,0,0,Y,abc;");
        CHECK(bad.size() == 2 && bad[0].squelchDb == kAirbandSquelchGlobal && bad[1].squelchDb == kAirbandSquelchGlobal, "damaged squelch ignored");
    }
    return testSummary("airband_store");
}
