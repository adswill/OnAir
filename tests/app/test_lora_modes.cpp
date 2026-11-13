// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRaWAN (all channels, family 200) and LoRa satellites (family 201): the tunings are registered under their engine standard numbers, the mode rows,
// the family and poll-slot counts, the engine wiring and the panels' privacy / honesty strings are in the sources.
#include <cstdlib>
#include "dect2/engine/mode_tuning.h"
#include "dect2/lorasat/lorasat_rx.h"
#include "dect2/lorawan/lorawan_rx.h"
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
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

int main() {
    CHECK(dect2::lorawanTuning().stdMode == 202 && dect2::loraSatTuning().stdMode == 203, "engine standards");
    const dect2::ModeTuning* a = dect2::modeTuning(202);
    const dect2::ModeTuning* b = dect2::modeTuning(203);
    CHECK(a && std::string(a->id) == "lorawanwide", "LoRaWAN tuning not registered");
    CHECK(b && std::string(b->id) == "lorasat", "LoRa satellite tuning not registered");
    CHECK(dect2::modeTuningById("lorasat") != nullptr && dect2::modeTuningById("lorawanwide") != nullptr, "ids");
    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(has(table, "{200, \"LoRaWAN (all channels)\", 8,") && has(table, "{201, \"LoRa satellites\", 4,"), "mode rows missing");
    const std::string appH = readFile("app/app.h");
    { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(appH.c_str() + q + 29) >= 202, "kNumFamilies does not cover family 201"); }
    const std::string eng = readFile("core/include/dect2/engine/engine.h");
    { const size_t q = eng.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(eng.c_str() + q + 16) >= 162, "kPollSeqSlots does not cover slot 161"); }
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(has(tel, "case 201: return pollStd(rxLw_, 201, t, seqs[160]") && has(tel, "case 202: return pollStd(rxLs_, 202, t, seqs[161]"), "telemetry cases");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "lorawanTuning(), loraSatTuning()") && has(modes, "case 202: return makeLorawanSynth") && has(modes, "case 203: return makeLoraSatSynth"), "modes.cpp wiring");
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(has(modeUi, "case 200: return &kLorawanUi;") && has(modeUi, "case 201: return &kLoraSatUi;"), "panels not mapped");
    const std::string cm = readFile("CMakeLists.txt");
    CHECK(has(cm, " lorawan lorasat "), "modes missing in CMakeLists.txt");
    const std::string lw = readFile("app/lorawan/lorawan_ui.cpp");
    for (const char* s : {"Receive only and keyless", "never decrypts", "never checks a MIC", "DevAddr", "FPort", "Gateway downlinks"}) CHECK(has(lw, s), "LoRaWAN panel string missing: %s", s);
    const std::string ls = readFile("app/lorasat/lorasat_ui.cpp");
    for (const char* s : {"unverified", "Doppler", "Presets", "Receive only"}) CHECK(has(ls, s), "LoRa satellite panel string missing: %s", s);
    const std::string passes = readFile("app/panels/passes_ui.cpp");
    CHECK(has(passes, "kLoraSat") && has(passes, "lorasat().setTracked"), "pass planner not wired");
    std::printf("%s: %d failure(s)\n", fails ? "test_lora_modes FAILED" : "test_lora_modes OK", fails);
    return fails ? 1 : 0;
}
