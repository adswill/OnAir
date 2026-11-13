// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ROS (HF) wiring: the tuning is registered under its engine standard, the mode row, family and poll-slot counts, the engine cases, the panel hook,
// the doc page and the panel / mode row carry the honesty strings (partial, needs a public spec).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/roshf/roshf_rx.h"

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
    const dect2::ModeTuning mt = dect2::roshfTuning();
    CHECK(mt.stdMode == 207 && dect2::modeTuningById("roshf") && dect2::modeTuningById("roshf")->stdMode == 207 && dect2::modeTuning(207), "tuning table");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "{205, \"ROS (HF)\", 10,"), "mode row");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 205: return &kRoshfUi;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 206, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 183, "kPollSeqSlots"); }
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(has(tel, "case 206: return pollStd(rxRoshf_, 206, t, seqs[182]"), "telemetry case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "roshfTuning()") && has(modes, "case 207: return makeRoshfSynth") && has(modes, "case 206: return roshfSummary"), "modes.cpp wiring");
    const std::string doc = readFile("docs/roshf.md");
    CHECK(has(doc, "partial, needs a public spec") && has(doc, "not published") && has(doc, "ROS"), "the help page says partial and why");
    const std::string ui = readFile("app/roshf/roshf_ui.cpp");
    CHECK(has(ui, "Partial") && has(ui, "needs a public spec"), "the panel says partial");
    const std::string tbl = readFile("app/ui/mode_table.cpp");
    CHECK(has(tbl, "needs a public spec") && has(tbl, "NOT decoded"), "mode row names the limit");
    const std::string num = readFile("docs/MODE-NUMBERS.md");
    CHECK(has(num, "| ROS (HF detector) | 205 | 207 | 206 | 182 |"), "docs/MODE-NUMBERS.md row");
    std::printf(fails ? "roshf mode: %d FAILED\n" : "roshf mode: all passed\n", fails);
    return fails ? 1 : 0;
}
