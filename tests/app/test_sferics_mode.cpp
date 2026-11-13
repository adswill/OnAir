// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sferics wiring: the tuning, the mode row, family and poll-slot counts, the engine cases, the panel hook and the honest doc page.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/sferics/sferics_rx.h"

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
    const dect2::ModeTuning mt = dect2::sfericsTuning();
    CHECK(mt.stdMode == 245 && dect2::modeTuningById("sferics") && dect2::modeTuningById("sferics")->stdMode == 245 && dect2::modeTuning(245), "tuning table");
    CHECK(mt.minSampleRate <= 48000 && mt.maxMhz >= 30, "works from a 48 kHz sound card to 30 MHz");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "{243, \"Sferics (lightning)\""), "mode row");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 243: return &kSfericsUi;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 244, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 204, "kPollSeqSlots"); }
    CHECK(has(readFile("core/src/engine/engine_telemetry.cpp"), "case 244: return pollStd(rxSfer_, 244, t, seqs[203]"), "telemetry case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "sfericsTuning()") && has(modes, "case 245: return makeSfericsSynth") && has(modes, "case 244: return sfericsSummary"), "modes.cpp wiring");
    CHECK(has(readFile("docs/MODE-NUMBERS.md"), "| Sferics (lightning) | 243 | 245 | 244 | 203 |"), "docs/MODE-NUMBERS.md row");
    const std::string ui = readFile("app/sferics/sferics_ui.cpp");
    CHECK(has(ui, "THRESHOLD") && has(ui, "Ignore mains interference") && has(ui, ".command = command"), "UI: options and the Simple view");
    const std::string doc = readFile("docs/SFERICS-3.7.md");
    CHECK(has(doc, "not where") && has(doc, "synthetic") && has(doc, "Blitzortung"), "the doc says what it does not do");
    std::printf(fails ? "sferics mode: %d FAILED\n" : "sferics mode: all passed\n", fails);
    return fails ? 1 : 0;
}
