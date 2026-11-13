// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sub-GHz sensors wiring: the tuning (433.92 MHz, 250 kHz), the mode row, family and poll-slot counts, the engine cases, the panel hook,
// the credits (rtl_433 is GPL-2.0-or-later code that ships in the program) and the doc page's honest wording and test numbers.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/sensors/sensors_rx.h"

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
    const dect2::ModeTuning mt = dect2::sensorsTuning();
    CHECK(mt.stdMode == 243 && dect2::modeTuningById("sensors") && dect2::modeTuningById("sensors")->stdMode == 243 && dect2::modeTuning(243), "tuning table");
    CHECK(mt.defMhz == 433.92 && mt.sampleRate == 250e3 && mt.minMhz <= 315 && mt.maxMhz >= 915, "433.92 MHz at 250 kHz, range 315 to 915 MHz");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "{241, \"Sub-GHz sensors\""), "mode row");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 241: return &kSensorsUi;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 242, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 202, "kPollSeqSlots"); }
    CHECK(has(readFile("core/src/engine/engine_telemetry.cpp"), "case 242: return pollStd(rxSens_, 242, t, seqs[201]"), "telemetry case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "sensorsTuning()") && has(modes, "case 243: return makeSensorsSynth") && has(modes, "case 242: return sensorsSummary"), "modes.cpp wiring");
    CHECK(has(readFile("THIRD_PARTY.md"), "rtl_433") && has(readFile("app/panels/credits_data.h"), "rtl_433"), "credits in THIRD_PARTY.md and in the app");
    CHECK(has(readFile("third_party/rtl_433/COPYING"), "GNU GENERAL PUBLIC LICENSE"), "rtl_433 licence text kept");
    const std::string doc = readFile("docs/SENSORS-3.7.md");
    CHECK(has(doc, "does not decode \"every sensor\"") && has(doc, "rtl_433_tests") && has(doc, "2946") && has(doc, "What this does **not** show"), "the doc says what is and is not supported, with the measured numbers");
    CHECK(has(readFile("docs/MODE-NUMBERS.md"), "| Sub-GHz sensors | 241 | 243 | 242 | 201 |"), "docs/MODE-NUMBERS.md row");
    CHECK(has(readFile("app/sensors/sensors_ui.cpp"), "Unknown signals") && has(readFile("app/sensors/sensors_ui.cpp"), "Remove after"), "UI has the analyser and Remove after");
    std::printf(fails ? "sensors mode: %d FAILED\n" : "sensors mode: all passed\n", fails);
    return fails ? 1 : 0;
}
