// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Yaesu Fusion mode (work in progress): the engine tuning is registered under its number, the mode row, the family count and the screen
// mapping agree with it, the engine is wired for it, and the panel's strings are in the screen source (text checks).
#include "dect2/engine/mode_tuning.h"
#include "dect2/fusion/fusion_rx.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)   // Flawfinder: ignore (literal formats)

static std::string readFile(const std::string& rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

int main() {
    const dect2::ModeTuning t = dect2::fusionTuning();
    CHECK(t.stdMode == 130, "stdMode %d", t.stdMode);
    const dect2::ModeTuning* reg = dect2::modeTuning(130);
    CHECK(reg != nullptr, "modeTuning(130) not registered");
    if (reg) CHECK(std::string(reg->name) == "Yaesu Fusion" && std::string(reg->id) == "fusion", "registered as '%s'", reg->name);
    CHECK(dect2::modeTuningById("fusion") != nullptr, "not found by id");

    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(has(table, "{128, \"Yaesu Fusion\", 11,"), "mode row for family 128 missing (group 11, with D-STAR and M17)");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 128: return &kFusionUi;"), "family 128 not mapped to kFusionUi");
    const std::string appH = readFile("app/app.h");
    const size_t at = appH.find("constexpr int kNumFamilies = ");
    CHECK(at != std::string::npos && std::atoi(appH.c_str() + at + 29) >= 129, "kNumFamilies below 129");
    const std::string lists = readFile("CMakeLists.txt");
    size_t count = 0, pos = 0;
    while ((pos = lists.find("set(DECT2_MODES", pos)) != std::string::npos) { count++; pos++; }
    CHECK(count == 1, "%zu DECT2_MODES lists", count);
    CHECK(has(lists, " fusion "), "fusion not in DECT2_MODES");

    // the engine: reset, feed, configure, offset, telemetry (the activeStd is the engine standard minus one: 129)
    CHECK(has(readFile("core/src/engine/engine.cpp"), "else if (a == 129) rxFusion_.feed(x, n);"), "feed");
    CHECK(has(readFile("core/src/engine/engine.cpp"), "case 129: rxFusion_.reset(); break;"), "reset");
    CHECK(has(readFile("core/src/engine/engine_place.cpp"), "case 129: rxFusion_.configure(r); break;"), "configure");
    CHECK(has(readFile("core/src/engine/engine_telemetry.cpp"), "case 129: return pollStd(rxFusion_, 129,"), "telemetry");
    CHECK(has(readFile("core/src/engine/modes.cpp"), "fusionTuning()"), "tuning list");

    const std::string ui = readFile("app/fusion/fusion_ui.cpp");
    const char* strings[] = {"Yaesu Fusion", "Position / slow data text", "Call mode: %s", "Repeater downlink", "Repeater uplink", "Source", "Destination",
                             "no audio until the codec above is ready", "speechCodecGate(\"ysf\"", "a.rx.standard == 129", "Data channel units"};
    for (const char* s : strings) CHECK(has(ui, s), "panel string missing: %s", s);

    // The codec must load from the per-frame tick (any tab), not only while the tab is drawn.
    const size_t t0 = ui.find("void tick(App& a)");
    CHECK(t0 != std::string::npos && has(ui.substr(t0, ui.find("void codecBlock") - t0), "speechCodecPoll(\"ysf\""), "ysf codec not loaded in tick");
    const std::string tet = readFile("app/tetra/tetra_ui.cpp");
    const size_t t1 = tet.find("void tick(App& a)");
    CHECK(t1 != std::string::npos && has(tet.substr(t1, tet.find("void tab(App& a)") - t1), "speechCodecPoll(\"tetra\""), "tetra codec not loaded in tick");

    std::printf("%s: %d failure(s)\n", fails ? "test_fusion_mode FAILED" : "test_fusion_mode OK", fails);
    return fails ? 1 : 0;
}
