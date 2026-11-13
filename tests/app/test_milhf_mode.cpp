// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF waveform ID mode: the engine tuning is registered under its number, the mode row and the family count agree with it,
// and the panel's strings (including the "identification only" labels) are in the screen source.
#include <cstdlib>
#include "dect2/milhf/milhf_engine.h"
#include "dect2/engine/mode_tuning.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { std::printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

static std::string readFile(const std::string& rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    const dect2::ModeTuning t = dect2::milhfTuning();
    CHECK(t.stdMode == 147, "stdMode %d, want 147", t.stdMode);
    const dect2::ModeTuning* reg = dect2::modeTuning(147);
    CHECK(reg != nullptr, "modeTuning(147) not registered");
    if (reg) CHECK(std::string(reg->name) == "HF waveform ID", "registered name '%s'", reg->name);

    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(table.find("{145, \"HF waveform ID\", 12,") != std::string::npos, "mode row for family 145 missing");
    const std::string appH = readFile("app/app.h");
    { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(appH.c_str() + q + 29) >= 146, "kNumFamilies does not cover family 145"); }
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(modeUi.find("case 145: return &kMilhfUi;") != std::string::npos, "family 145 not mapped to kMilhfUi");
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(tel.find("case 146: return pollStd(rxMilhf_, 146, t, seqs[135]") != std::string::npos, "telemetry case 146 missing");
    const std::string eng = readFile("core/include/dect2/engine/engine.h");
    { const size_t q = eng.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(eng.c_str() + q + 16) >= 136, "kPollSeqSlots does not cover slot 135"); }

    const std::string ui = readFile("app/milhf/milhf_ui.cpp");
    const char* strings[] = {"Identification only", "estimates", "Save audio clip for sigidwiki", "Candidates", "Measured parameters", "Occupied band"};
    for (const char* s : strings)
        CHECK(ui.find(s) != std::string::npos, "panel string missing: %s", s);

    std::printf("%s: %d failure(s)\n", fails ? "test_milhf_mode FAILED" : "test_milhf_mode OK", fails);
    return fails ? 1 : 0;
}
