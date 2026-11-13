// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Channelised voice modes (families 340 - 346): the engine tunings are registered under their numbers, the mode rows, the family count, the
// screen mapping, the engine cases and the test-signal cases agree.
#include "dect2/chanvoice/chv_plan.h"
#include "dect2/chanvoice/chv_rx.h"
#include "dect2/engine/mode_tuning.h"
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
    using namespace dect2;
    const char* ids[] = {"cb", "frs", "pmr446", "hamfm", "hfvoice", "mdc1200", "wmic"};
    for (int i = 0; i < chv::kServiceCount; i++) {
        const int fam = 340 + i;
        const ModeTuning* m = modeTuning(fam + 2);
        CHECK(m && std::string(m->id) == ids[i], "family %d: tuning id", fam);
        CHECK(modeTuningById(ids[i]) != nullptr, "id %s not registered", ids[i]);
        CHECK(m && m->stdMode == fam + 2 && m->minSampleRate <= m->sampleRate, "family %d: rates", fam);
        CHECK(chanVoiceTuning((chv::Service)i).stdMode == fam + 2, "tuning function %d", i);
        CHECK(chv::familyOf((chv::Service)i) == fam, "familyOf %d", i);
    }
    const std::string table = readFile("app/ui/mode_table.cpp");
    for (const char* n : {"{340, \"CB radio\", 5,", "{341, \"FRS / GMRS\", 5,", "{342, \"PMR446 analog\", 5,", "{343, \"Ham FM repeaters\", 11,", "{344, \"HFGCS / HF voice\", 12,", "{345, \"MDC-1200\", 5,", "{346, \"Wireless mic finder\", 5,"})
        CHECK(table.find(n) != std::string::npos, "mode row missing: %s", n);
    const std::string appH = readFile("app/app.h");
    { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(appH.c_str() + q + 29) >= 347, "kNumFamilies does not cover family 346"); }
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(modeUi.find("case 340: case 341: case 342: case 343: case 344: case 345: case 346: return &kChanVoiceUi;") != std::string::npos, "families 340 - 346 not mapped");
    const std::string cm = readFile("CMakeLists.txt");
    CHECK(cm.find("DECT2_MODES chanvoice") != std::string::npos, "chanvoice not in DECT2_MODES");
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(tel.find("case 341: case 342: case 343: case 344: case 345: case 346: case 347: return pollStd(rxChv_, a,") != std::string::npos, "engine poll case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(modes.find("case 342: case 343: case 344: case 345: case 346: case 347: case 348: return makeChanVoiceSynth") != std::string::npos, "test signal cases");
    const std::string ui = readFile("app/chanvoice/chanvoice_ui.cpp");
    for (const char* s : {"Automatic: the strongest open channel", "Auto: follow the location setting", "MDC-1200 IDs", "Call log", "Record##chvrec", "FleetSync (Kenwood) is not decoded"})
        CHECK(ui.find(s) != std::string::npos, "screen string missing: %s", s);
    std::printf("%s: %d failure(s)\n", fails ? "test_chanvoice_mode FAILED" : "test_chanvoice_mode OK", fails);
    return fails ? 1 : 0;
}
