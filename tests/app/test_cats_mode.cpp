// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CATS wiring: the tuning is registered under its engine standard, the mode row, the panel hook, the engine cases and the numbers table.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/cats/cats_rx.h"

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
    const dect2::ModeTuning mt = dect2::catsTuning();
    CHECK(mt.stdMode == 902 && dect2::modeTuningById("cats") && dect2::modeTuningById("cats")->stdMode == 902 && dect2::modeTuning(902), "tuning table");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "{900, \"CATS\", 25,"), "mode row");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 900: return &kCatsUi;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 901, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 901, "kPollSeqSlots"); }
    CHECK(has(readFile("core/src/engine/engine_telemetry.cpp"), "case 901: return pollStd(rxCats_, 901, t, seqs[900]"), "telemetry case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "catsTuning()") && has(modes, "case 902: return makeCatsSynth") && has(modes, "case 901: return catsSummary"), "modes.cpp wiring");
    const std::string eng = readFile("core/src/engine/engine.cpp");
    CHECK(has(eng, "if (a == 901) { rxCats_.feed(x, n); return; }") && has(eng, "case 901: rxCats_.reset(); break;"), "engine feed and reset");
    CHECK(has(readFile("docs/MODE-NUMBERS.md"), "| CATS | 900 | 902 | 901 | 900 |"), "numbers table");
    CHECK(has(readFile("MODES.md"), "CATS"), "MODES.md entry");
    std::printf(fails ? "cats mode: %d FAILED\n" : "cats mode: all passed\n", fails);
    return fails ? 1 : 0;
}
