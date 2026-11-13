// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SBAS mode: the engine tuning is registered under its number, the mode row and the family count agree with it,
// and the panel's strings are in the screen source.
#include <cstdlib>
#include "dect2/sbas/sbas_engine.h"
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
    const dect2::ModeTuning t = dect2::sbasTuning();
    CHECK(t.stdMode == 152, "stdMode %d, want 152", t.stdMode);
    const dect2::ModeTuning* reg = dect2::modeTuning(152);
    CHECK(reg != nullptr, "modeTuning(152) not registered");
    if (reg) CHECK(std::string(reg->name) == "SBAS", "registered name '%s'", reg->name);

    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(table.find("{150, \"SBAS\", ") != std::string::npos, "mode row for family 150 missing");
    const std::string appH = readFile("app/app.h");
    { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(appH.c_str() + q + 29) >= 151, "kNumFamilies does not cover family 150"); }
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(modeUi.find("case 150: return &kSbasUi;") != std::string::npos, "family 150 not mapped to kSbasUi");
    const std::string eng = readFile("core/include/dect2/engine/engine.h");
    { const size_t q = eng.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(eng.c_str() + q + 16) >= 152, "kPollSeqSlots does not cover the slot"); }
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(tel.find("case 151: return pollStd(rxSbas_, 151, t, seqs[150]") != std::string::npos, "telemetry case missing");

    const std::string ui = readFile("app/sbas/sbas_ui.cpp");
    const char* strings[] = {"Fast corrections", "Ionosphere", "Satellites", "PRN mask", "not applied"};
    for (const char* s : strings)
        CHECK(ui.find(s) != std::string::npos, "panel string missing: %s", s);

    std::printf("%s: %d failure(s)\n", fails ? "test_sbas_mode FAILED" : "test_sbas_mode OK", fails);
    return fails ? 1 : 0;
}
