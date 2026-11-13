// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SSDV wiring: the tuning is registered under its engine standard, the mode row, family and poll-slot counts, the engine cases, the panel hook,
// the doc page and the panel / mode row carry the honesty strings (GPL notice, limits).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/ssdv/ssdv_rx.h"

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
    const dect2::ModeTuning mt = dect2::ssdvTuning();
    CHECK(mt.stdMode == 208 && dect2::modeTuningById("ssdv") && dect2::modeTuningById("ssdv")->stdMode == 208 && dect2::modeTuning(208), "tuning table");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "{206, \"SSDV pictures\", 9,"), "mode row");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 206: return &kSsdvUi;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 207, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 184, "kPollSeqSlots"); }
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(has(tel, "case 207: return pollStd(rxSsdv_, 207, t, seqs[183]"), "telemetry case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "ssdvTuning()") && has(modes, "case 208: return makeSsdvSynth") && has(modes, "case 207: return ssdvSummary"), "modes.cpp wiring");
    const std::string doc = readFile("docs/ssdv.md");
    CHECK(has(doc, "GPL-3.0") && has(doc, "fsphil") && has(doc, "Wenet") && has(doc, "not handled") && has(doc, "user test with a real recording"), "the help page names the licence and the limits");
    CHECK(has(readFile("THIRD_PARTY.md"), "fsphil/ssdv"), "THIRD_PARTY.md names ssdv");
    const std::string ui = readFile("app/ssdv/ssdv_ui.cpp");
    CHECK(has(ui, "SSDV") && has(ui, "Save PNG"), "the panel");
    const std::string tbl = readFile("app/ui/mode_table.cpp");
    CHECK(has(tbl, "needs a user test with a real recording") && has(tbl, "LoRa are not handled") && has(tbl, "GPL-3.0-or-later"), "mode row names the limits and the licence");
    const std::string num = readFile("docs/MODE-NUMBERS.md");
    CHECK(has(num, "| SSDV pictures | 206 | 208 | 207 | 183 |"), "docs/MODE-NUMBERS.md row");
    std::printf(fails ? "ssdv mode: %d FAILED\n" : "ssdv mode: all passed\n", fails);
    return fails ? 1 : 0;
}
