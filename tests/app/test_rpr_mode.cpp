// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Robust Packet wiring: the tuning is registered under its engine standard, the mode row, family and poll-slot counts, the engine cases, the panel
// hook, the help page and the panel / mode row say "physical layer only" and that the coding is not decoded (needs a recording).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/rpr/rpr_rx.h"

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
    const dect2::ModeTuning mt = dect2::rprTuning();
    CHECK(mt.stdMode == 901 && dect2::modeTuningById("rpr") && dect2::modeTuningById("rpr")->stdMode == 901 && dect2::modeTuning(901), "tuning table");
    const std::string tbl = readFile("app/ui/mode_table.cpp");
    CHECK(has(tbl, "{899, \"Robust Packet (RPR, needs a user test)\", 12,"), "mode row");
    CHECK(has(tbl, "needs a recording") && has(tbl, "not public"), "mode row names the limit");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 899: return &kRprUi;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 900, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 900, "kPollSeqSlots"); }
    CHECK(has(readFile("core/src/engine/engine_telemetry.cpp"), "case 900: return pollStd(rxRpr_, 900, t, seqs[899]"), "telemetry case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "rprTuning()") && has(modes, "case 901: return makeRprSynth") && has(modes, "case 900: return rprSummary"), "modes.cpp wiring");
    const std::string eng = readFile("core/src/engine/engine.cpp");
    CHECK(has(eng, "if (a == 900) { rxRpr_.feed(x, n); return; }") && has(eng, "case 900: rxRpr_.reset(); break;"), "engine feed and reset");
    const std::string doc = readFile("docs/rpr.md");
    CHECK(has(doc, "physical layer") && has(doc, "not public") && has(doc, "needs a recording"), "the help page says physical layer only and names the limit");
    const std::string ui = readFile("app/rpr/rpr_ui.cpp");
    CHECK(has(ui, "Physical layer only") && has(ui, "needs a recording"), "the panel says physical layer only");
    CHECK(has(readFile("docs/MODE-NUMBERS.md"), "| Robust Packet (RPR) | 899 | 901 | 900 | 899 |"), "numbers table");
    CHECK(has(readFile("MODES.md"), "Robust Packet (RPR"), "MODES.md entry");
    std::printf(fails ? "rpr mode: %d FAILED\n" : "rpr mode: all passed\n", fails);
    return fails ? 1 : 0;
}
