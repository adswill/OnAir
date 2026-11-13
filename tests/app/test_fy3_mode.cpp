// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FY-3 AHRPT / MPT wiring: the tunings are registered under their engine standards, the mode rows, family and poll-slot counts, the engine cases, the panel hooks,
// the doc page, and the screens say what the modes need.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/fy3/fy3_rx.h"

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
    const dect2::ModeTuning a = dect2::fy3AhrptTuning(), m = dect2::fy3MptTuning();
    CHECK(a.stdMode == 237 && dect2::modeTuningById("fy3ahrpt") && dect2::modeTuningById("fy3ahrpt")->stdMode == 237 && dect2::modeTuning(237), "AHRPT tuning table");
    CHECK(m.stdMode == 238 && dect2::modeTuningById("fy3mpt") && dect2::modeTuningById("fy3mpt")->stdMode == 238 && dect2::modeTuning(238), "MPT tuning table");
    CHECK(a.defMhz >= 1701.3 && a.defMhz <= 1704.6 && a.minSampleRate >= 4e6 && a.minSampleRate <= 6e6, "AHRPT band and rate");
    CHECK(m.defMhz >= 7700 && m.defMhz <= 7800, "MPT band");
    int n;
    const dect2::Fy3Preset* pa = dect2::fy3Presets(0, n);
    CHECK(n >= 2 && pa[0].mhz == 1701.4 && pa[0].symbolRate == 2.6e6 && pa[1].mhz == 1704.5 && pa[1].symbolRate == 2.8e6, "AHRPT presets");
    const dect2::Fy3Preset* pm = dect2::fy3Presets(1, n);
    CHECK(n >= 2 && pm[0].mhz == 7750 && pm[0].symbolRate == 18.7e6, "MPT presets");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "{235, \"FY-3 AHRPT\"") && has(readFile("app/ui/mode_table.cpp"), "{236, \"FY-3 MPT\""), "mode rows");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 235: return &kFy3AhrptUi;") && has(readFile("app/panels/modeui.cpp"), "case 236: return &kFy3MptUi;"), "panel hooks");
    { const std::string s = readFile("app/app.h"); const size_t q = s.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(s.c_str() + q + 29) >= 237, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 197, "kPollSeqSlots"); }
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(has(tel, "case 236: return pollStd(rxFy3a_, 236, t, seqs[195]") && has(tel, "case 237: return pollStd(rxFy3m_, 237, t, seqs[196]"), "telemetry cases");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "fy3AhrptTuning()") && has(modes, "fy3MptTuning()") && has(modes, "case 237: return makeFy3Synth") && has(modes, "case 238: return makeFy3Synth") && has(modes, "case 236: return fy3Summary"), "modes.cpp wiring");
    const std::string doc = readFile("docs/fy3.md");
    CHECK(has(doc, "1701.4") && has(doc, "1704.5") && has(doc, "7750") && has(doc, "18.7") && has(doc, "SatDump") && has(doc, "VIRR") && has(doc, "MERSI"), "the help page names the downlinks, the instruments and the source");
    CHECK(has(readFile("docs/MODE-NUMBERS.md"), "| FY-3 AHRPT | 235 | 237 | 236 | 195 |") && has(readFile("docs/MODE-NUMBERS.md"), "| FY-3 MPT | 236 | 238 | 237 | 196 |"), "docs/MODE-NUMBERS.md rows");
    const std::string ui = readFile("app/fy3/fy3_ui.cpp");
    CHECK(has(ui, "X-band") && has(ui, "down-converter") && has(ui, "LNA"), "the panel says what the modes need");
    CHECK(has(readFile("THIRD_PARTY.md"), "SatDump (FengYun-3 plug-in)"), "THIRD_PARTY.md credit");
    CHECK(has(readFile("app/panels/credits_data.h"), "SatDump (FengYun-3 plug-in)"), "in-app credit");
    std::printf(fails ? "fy3 mode: %d FAILED\n" : "fy3 mode: all passed\n", fails);
    return fails ? 1 : 0;
}
