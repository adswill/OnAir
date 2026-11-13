// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// eLoran / Loran-C mode : the engine tuning is registered under its number, the mode row and the family
// count agree with it, and the panel's strings are in the screen source.
#include "dect2/eloran/eloran_engine.h"
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
    const dect2::ModeTuning t = dect2::elLoranTuning();
    CHECK(t.stdMode == 128, "stdMode %d, want 128", t.stdMode);
    CHECK(t.minMhz < 0.1 && t.maxMhz > 0.1 && t.minSampleRate >= 250000, "tuning range or sample rate");
    const dect2::ModeTuning* reg = dect2::modeTuning(128);
    CHECK(reg != nullptr, "modeTuning(128) not registered");
    if (reg) CHECK(std::string(reg->name) == "eLoran / Loran-C", "registered name '%s'", reg->name);

    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(table.find("{126, \"eLoran / Loran-C\", 2,") != std::string::npos, "mode row for family 126 missing");
    const std::string appH = readFile("app/app.h");
    CHECK(appH.find("constexpr int kNumFamilies = ") != std::string::npos, "kNumFamilies missing");
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(modeUi.find("case 126: return &kElLoranUi;") != std::string::npos, "family 126 not mapped to kElLoranUi");
    const std::string cmake = readFile("CMakeLists.txt");
    CHECK(cmake.find(" eloran ") != std::string::npos, "eloran not in DECT2_MODES");
    size_t lists = 0;
    for (size_t p = cmake.find("set(DECT2_MODES"); p != std::string::npos; p = cmake.find("set(DECT2_MODES", p + 1)) lists++;
    CHECK(lists == 1, "%d DECT2_MODES lists, want exactly 1", (int)lists);

    const std::string eng = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(eng.find("case 127: return pollStd(rxEloran_, 127,") != std::string::npos, "telemetry case 127 missing");

    const std::string ui = readFile("app/eloran/eloran_ui.cpp");
    const char* strings[] = {"eLoran / Loran-C", "ToA offset (us)", "Level (dBFS)", "Phase code", "Averaged pulse",
                             "tune to 100 kHz (90 - 110 kHz) and start the receiver"};
    for (const char* s : strings) CHECK(ui.find(s) != std::string::npos, "panel string missing: %s", s);

    std::printf("%s: %d failure(s)\n", fails ? "test_eloran_mode FAILED" : "test_eloran_mode OK", fails);
    return fails ? 1 : 0;
}
