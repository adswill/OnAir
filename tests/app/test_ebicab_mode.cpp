// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EBICab / Eurobalise wiring: the tuning is registered under its engine standard, the mode row, family and poll-slot counts, the engine cases, the panel hook,
// the doc page and the panel / mode row carry the honesty strings (needs a user test, EBICab formats not public).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/ebicab/ebicab_rx.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static std::string readFile(const std::string& rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

int main() {
    const dect2::ModeTuning mt = dect2::ebicabTuning();
    CHECK(mt.stdMode == 209 && dect2::modeTuningById("ebicab") && dect2::modeTuningById("ebicab")->stdMode == 209 && dect2::modeTuning(209), "tuning table");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "{207, \"EBICab / Eurobalise (needs a user test)\", 6,"), "mode row");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 207: return &kEbicabUi;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 208, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 185, "kPollSeqSlots"); }
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(has(tel, "case 208: return pollStd(rxEbi_, 208, t, seqs[184]"), "telemetry case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "ebicabTuning()") && has(modes, "case 209: return makeEbicabSynth") && has(modes, "case 208: return ebicabSummary"), "modes.cpp wiring");
    const std::string doc = readFile("docs/ebicab.md");
    CHECK(has(doc, "needs a user test") && has(doc, "SUBSET-036") && has(doc, "not published") && has(doc, "27.095"), "the help page says what is and is not decoded");
    const std::string ui = readFile("app/ebicab/ebicab_ui.cpp");
    CHECK(has(ui, "Needs a user test") && has(ui, "not public"), "the panel says needs a user test");
    const std::string tbl = readFile("app/ui/mode_table.cpp");
    CHECK(has(tbl, "needs a user test") && has(tbl, "EBICab 700 (32 bit) and 900 (255 bit) telegram formats are not published"), "mode row names the limit");
    const std::string num = readFile("docs/MODE-NUMBERS.md");
    CHECK(has(num, "| EBICab / Eurobalise | 207 | 209 | 208 | 184 |"), "docs/MODE-NUMBERS.md row");
    std::printf(fails ? "ebicab mode: %d FAILED\n" : "ebicab mode: all passed\n", fails);
    return fails ? 1 : 0;
}
