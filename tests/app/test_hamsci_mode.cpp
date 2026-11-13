// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham science wiring: tuning for every kind, mode rows, panel hook, family and poll-slot counts, the engine cases, the number table, and an honest doc page
// (simulated signals only, what antenna the pulsar mode needs, which of the requested modes were not built and why).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/hamsci/hamsci_rx.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static std::string readFile(const std::string& rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static bool has(const std::string& s, const std::string& t) { return s.find(t) != std::string::npos; }

int main() {
    const std::string table = readFile("app/ui/mode_table.cpp"), modeui = readFile("app/panels/modeui.cpp"), tel = readFile("core/src/engine/engine_telemetry.cpp"), modes = readFile("core/src/engine/modes.cpp");
    const std::string nums = readFile("docs/MODE-NUMBERS.md"), place = readFile("core/src/engine/engine_place.cpp"), eng = readFile("core/src/engine/engine.cpp");
    for (int k = 0; k < dect2::kHamsciKinds; k++) {
        const int fam = dect2::hamsciFamily(k);
        const dect2::ModeTuning mt = dect2::hamsciTuning(k);
        CHECK(mt.stdMode == fam + 2 && dect2::modeTuning(fam + 2) && dect2::modeTuningById(dect2::hamsciId(k)) && dect2::modeTuningById(dect2::hamsciId(k))->stdMode == fam + 2, "tuning table kind %d", k);
        CHECK(has(table, "{" + std::to_string(fam) + ", \""), "mode row %d", fam);
        CHECK(has(tel, "case " + std::to_string(fam + 1) + ": return pollStd(rxHs_[" + std::to_string(k) + "], " + std::to_string(fam + 1) + ", t, seqs[" + std::to_string(fam + 1) + "]"), "telemetry case %d", fam + 1);
        CHECK(has(modes, "case " + std::to_string(fam + 2) + ": return makeHamsciSynth(cfg, sampleRate, " + std::to_string(k) + ")"), "synth case %d", fam + 2);
        CHECK(has(modes, "case " + std::to_string(fam + 1) + ":"), "summary case %d", fam + 1);
        CHECK(has(place, "case " + std::to_string(fam + 1) + ": rxHs_[" + std::to_string(k) + "].configure(r)"), "configure case %d", fam + 1);
        CHECK(has(eng, "case " + std::to_string(fam + 1) + ": rxHs_[" + std::to_string(k) + "].reset()"), "reset case %d", fam + 1);
        CHECK(has(nums, "| " + std::to_string(fam) + " | " + std::to_string(fam + 2) + " | " + std::to_string(fam + 1) + " | " + std::to_string(fam + 1) + " |"), "number table row %d", fam);
        CHECK(dect2::hamsciKindOfFamily(fam) == k && dect2::hamsciKindOfStd(fam + 2) == k && dect2::hamsciKindOfActive(fam + 1) == k, "kind lookup %d", k);
    }
    CHECK(has(modeui, "case 460: case 461: case 462: case 463: case 464: case 465: return &kHamsciUi[family - 460];") && has(modeui, "case 500: case 501: case 502: return &kHamsciUi[6 + family - 500];"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 503, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 309, "kPollSeqSlots"); }
    CHECK(has(eng, "hamsciKindOfActive(a) >= 0) rxHs_[hamsciKindOfActive(a)].feed(x, n)"), "feed dispatch");
    const std::string ui = readFile("app/hamsci/hamsci_ui.cpp");
    CHECK(has(ui, "Auto") && has(ui, "ADVANCED") && has(ui, "What antenna this needs") && has(ui, ".command = [](App& a)"), "UI: Auto on top, manual under Advanced, the antenna statement, the Simple view");
    const std::string doc = readFile("docs/HAM-SCIENCE-3.8.md");
    CHECK(has(doc, "simulated") && has(doc, "Opera") && has(doc, "SIM31") && has(doc, "WINMOR") && has(doc, "square metre") && has(doc, "not tested on a real"), "the doc says what is simulated, what was skipped and what the pulsar mode needs");
    std::printf(fails ? "ham science mode wiring: %d FAILED\n" : "ham science mode wiring: all passed\n", fails);
    return fails ? 1 : 0;
}
