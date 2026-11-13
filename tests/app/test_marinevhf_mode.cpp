// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Marine VHF mode: the engine tuning is registered under its number, the mode row, the family count, the screen mapping and the DSC jump agree,
// and the panel strings are in the screen sources.
#include "dect2/engine/mode_tuning.h"
#include "dect2/marinevhf/marinevhf_rx.h"
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
    CHECK(dect2::marineVhfTuning().stdMode == 157, "std mode");
    const dect2::ModeTuning* m = dect2::modeTuning(157);
    CHECK(m && std::string(m->name) == "Marine VHF", "Marine VHF not registered");
    CHECK(dect2::modeTuningById("marinevhf") != nullptr, "id not registered");
    CHECK(m && m->minSampleRate <= 250000 && m->sampleRate >= 6.5e6, "rates");

    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(table.find("{155, \"Marine VHF\", 3,") != std::string::npos, "mode row for family 155 missing (Maritime group)");
    const std::string appH = readFile("app/app.h");
    { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(appH.c_str() + q + 29) >= 156, "kNumFamilies does not cover family 148"); }
    CHECK(appH.find("void marineVhfJump(App& a, int channel") != std::string::npos, "marineVhfJump not declared");
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(modeUi.find("case 155: return &kMarineVhfUi;") != std::string::npos, "family 155 not mapped");
    const std::string cm = readFile("CMakeLists.txt");
    CHECK(cm.find(" marinevhf ") != std::string::npos, "marinevhf not in DECT2_MODES");
    CHECK(cm.find("marine_vhf_channels.csv") != std::string::npos, "the channel table is not compiled in");
    const std::string ui = readFile("app/marinevhf/marinevhf_ui.cpp");
    for (const char* s : {"Watch channel 16", "Automatic: the strongest open channel", "Call log", "Record##mvrec", "Open the DSC mode", "Open the AIS mode", "return a.rx.standard == 156;"})
        CHECK(ui.find(s) != std::string::npos, "screen string missing: %s", s);
    const std::string marineUi = readFile("app/marine/marine_ui.cpp");
    CHECK(marineUi.find("Listen on VHF channel") != std::string::npos && marineUi.find("marineVhfJump(a,") != std::string::npos, "the DSC screen has no jump to the channel");
    const std::string engine = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(engine.find("case 156: return pollStd(rxMarineVhf_, 156") != std::string::npos, "engine poll case");

    std::printf("%s: %d failure(s)\n", fails ? "test_marinevhf_mode FAILED" : "test_marinevhf_mode OK", fails);
    return fails ? 1 : 0;
}
