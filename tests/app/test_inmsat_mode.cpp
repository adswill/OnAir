// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Inmarsat all-carriers wiring: the tuning is registered under its engine standard, the mode row, family and poll-slot counts, the engine cases, the
// panel hook, the doc page, and the honesty strings (needs a user test, C voice not decoded, codec never shipped).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/inmsat/inmsat_rx.h"

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
    const dect2::ModeTuning mt = dect2::inmsatTuning();
    CHECK(mt.stdMode == 210 && dect2::modeTuningById("inmsat") && dect2::modeTuningById("inmsat")->stdMode == 210 && dect2::modeTuning(210), "tuning table");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "{208, \"Inmarsat (all carriers)\", 4,"), "mode row");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 208: return &kInmsatUi;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 209, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 186, "kPollSeqSlots"); }
    CHECK(has(readFile("core/src/engine/engine_telemetry.cpp"), "case 209: return pollStd(rxInmsat_, 209, t, seqs[185]"), "telemetry case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "inmsatTuning()") && has(modes, "case 210: return makeInmsatSynth") && has(modes, "case 209: return inmsatSummary"), "modes.cpp wiring");
    const std::string doc = readFile("docs/INMARSAT.md");
    CHECK(has(doc, "needs a user test") && has(doc, "C channel") && has(doc, "never shipped") && has(doc, "R and T"), "the doc names the limits");
    const std::string tbl = readFile("app/ui/mode_table.cpp");
    CHECK(has(tbl, "needs a user test with a real capture") && has(tbl, "never shipped"), "mode row names the limits");
    const std::string ui = readFile("app/inmsat/inmsat_ui.cpp");
    CHECK(has(ui, "Carriers") && has(ui, "Messages") && has(ui, "Aircraft") && has(ui, "Voice") && has(ui, "Auto##inmsat"), "the panel has its tabs and the Auto button");
    CHECK(has(readFile("docs/MODE-NUMBERS.md"), "| Inmarsat (all carriers) | 208 | 210 | 209 | 185 |"), "docs/MODE-NUMBERS.md row");
    std::printf(fails ? "inmsat mode: %d FAILED\n" : "inmsat mode: all passed\n", fails);
    return fails ? 1 : 0;
}
