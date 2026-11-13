// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PACTOR-II / -III / -IV wiring: the tunings are registered under their engine standards, the mode rows, family and poll-slot counts, the engine cases,
// the panel hooks, the help page and the panel say "physical layer only" and that the coding is proprietary and not decoded.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/pactorhf/pactorhf_rx.h"

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
    const dect2::ModeTuning t2 = dect2::pactor2Tuning(), t3 = dect2::pactor3Tuning(), t4 = dect2::pactor4Tuning();
    CHECK(t2.stdMode == 239 && t3.stdMode == 240 && t4.stdMode == 241, "engine standards");
    CHECK(dect2::modeTuningById("pactor2") && dect2::modeTuningById("pactor2")->stdMode == 239 && dect2::modeTuning(239), "PACTOR-II tuning table");
    CHECK(dect2::modeTuningById("pactor3") && dect2::modeTuningById("pactor3")->stdMode == 240 && dect2::modeTuning(240), "PACTOR-III tuning table");
    CHECK(dect2::modeTuningById("pactor4") && dect2::modeTuningById("pactor4")->stdMode == 241 && dect2::modeTuning(241), "PACTOR-IV tuning table");
    const std::string tbl = readFile("app/ui/mode_table.cpp");
    CHECK(has(tbl, "{237, \"PACTOR-II\", 12,") && has(tbl, "{238, \"PACTOR-III\", 12,") && has(tbl, "{239, \"PACTOR-IV\", 12,"), "mode rows");
    CHECK(has(tbl, "proprietary (SCS) and not published, so no text is produced"), "mode rows name the limit");
    const std::string mu = readFile("app/panels/modeui.cpp");
    CHECK(has(mu, "case 237: return &kPactor2Ui;") && has(mu, "case 238: return &kPactor3Ui;") && has(mu, "case 239: return &kPactor4Ui;"), "panel hooks");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 240, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 200, "kPollSeqSlots"); }
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(has(tel, "case 238: return pollStd(rxPh2_, 238, t, seqs[197]") && has(tel, "case 239: return pollStd(rxPh3_, 239, t, seqs[198]") && has(tel, "case 240: return pollStd(rxPh4_, 240, t, seqs[199]"), "telemetry cases");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "pactor2Tuning(), pactor3Tuning(), pactor4Tuning()") && has(modes, "case 239: return makePactorHfSynth(2") && has(modes, "case 241: return makePactorHfSynth(4") && has(modes, "case 238: case 239: case 240: return pactorHfSummary"), "modes.cpp wiring");
    const std::string doc = readFile("docs/pactorhf.md");
    CHECK(has(doc, "physical layer") && has(doc, "proprietary") && has(doc, "not decoded"), "the help page says physical layer only and names the limit");
    const std::string ui = readFile("app/pactorhf/pactorhf_ui.cpp");
    CHECK(has(ui, "Physical layer only") && has(ui, "proprietary"), "the panel says physical layer only");
    const std::string md = readFile("docs/MODE-NUMBERS.md");
    CHECK(has(md, "| PACTOR-II | 237 | 239 | 238 | 197 |") && has(md, "| PACTOR-III | 238 | 240 | 239 | 198 |") && has(md, "| PACTOR-IV | 239 | 241 | 240 | 199 |"), "numbers table");
    std::printf(fails ? "pactorhf mode: %d FAILED\n" : "pactorhf mode: all passed\n", fails);
    return fails ? 1 : 0;
}
