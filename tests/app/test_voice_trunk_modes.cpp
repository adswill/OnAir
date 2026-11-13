// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The voice trunking modes of 3.8 (Motorola Type II, TETRA DMO, OpenSky identifier): the engine tunings are registered under their numbers, and the mode rows, the family count and the screen mapping agree.
#include "dect2/engine/mode_tuning.h"
#include "dect2/opensky/opensky_rx.h"
#include "dect2/smartnet/smartnet_rx.h"
#include "dect2/tetradmo/tetradmo_rx.h"
#include <cstdio>
#include <cstdlib>
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
    CHECK(dect2::smartnetTuning().stdMode == 322, "smartnet std");
    CHECK(dect2::tetraDmoTuning().stdMode == 323, "tetradmo std");
    CHECK(dect2::openSkyTuning().stdMode == 324, "opensky std");
    for (int std : {322, 323, 324}) CHECK(dect2::modeTuning(std) != nullptr, "std %d not registered", std);
    for (const char* id : {"smartnet", "tetradmo", "opensky"}) CHECK(dect2::modeTuningById(id) != nullptr, "%s not registered", id);
    const std::string table = readFile("app/ui/mode_table.cpp");
    for (const char* row : {"{320, \"Motorola Type II\", 5,", "{321, \"TETRA DMO\", 5,", "{322, \"OpenSky identifier\", 5,"}) CHECK(table.find(row) != std::string::npos, "row missing: %s", row);
    const std::string appH = readFile("app/app.h");
    { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(appH.c_str() + q + 29) >= 340, "kNumFamilies does not cover the block"); }
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    for (const char* c : {"case 320: return &kSmartnetUi;", "case 321: return &kTetraDmoUi;", "case 322: return &kOpenSkyUi;"}) CHECK(modeUi.find(c) != std::string::npos, "not mapped: %s", c);
    std::printf("%s: %d failure(s)\n", fails ? "test_voice_trunk_modes FAILED" : "test_voice_trunk_modes OK", fails);
    return fails ? 1 : 0;
}
