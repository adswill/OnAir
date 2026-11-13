// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ASTERIX mode: the engine tuning is registered under its number, the mode row and the family count agree with it,
// and the panel's strings are in the screen source.
#include <cstdlib>
#include "dect2/asterix/asterix_engine.h"
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
    const dect2::ModeTuning t = dect2::asterixTuning();
    CHECK(t.stdMode == 153, "stdMode %d, want 153", t.stdMode);
    const dect2::ModeTuning* reg = dect2::modeTuning(153);
    CHECK(reg != nullptr, "modeTuning(153) not registered");
    if (reg) CHECK(std::string(reg->name) == "ASTERIX", "registered name '%s'", reg->name);

    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(table.find("{151, \"ASTERIX\", ") != std::string::npos, "mode row for family 151 missing");
    const std::string appH = readFile("app/app.h");
    { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(appH.c_str() + q + 29) >= 152, "kNumFamilies does not cover family 151"); }
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(modeUi.find("case 151: return &kAsterixUi;") != std::string::npos, "family 151 not mapped to kAsterixUi");
    const std::string eng = readFile("core/include/dect2/engine/engine.h");
    { const size_t q = eng.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(eng.c_str() + q + 16) >= 153, "kPollSeqSlots does not cover the slot"); }

    const std::string ui = readFile("app/asterix/asterix_ui.cpp");
    const char* strings[] = {"Listen", "Open file...", "demo traffic", "Tracks", "Sources", "Records", "CAT 048", "approximate"};
    for (const char* s : strings)
        CHECK(ui.find(s) != std::string::npos, "panel string missing: %s", s);

    std::printf("%s: %d failure(s)\n", fails ? "test_asterix_mode FAILED" : "test_asterix_mode OK", fails);
    return fails ? 1 : 0;
}
