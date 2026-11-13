// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NFC / RFID mode wiring: the engine tuning is registered under its number, the mode row, family count and screen mapping agree
// with it, the engine dispatch and telemetry are in place, and the panel's strings are in the screen source.
#include "dect2/nfc/nfc_engine.h"
#include "dect2/engine/mode_tuning.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { std::printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

static std::string readFile(const std::string& rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss; ss << in.rdbuf(); return ss.str();
}
static bool has(const std::string& s, const std::string& n) { return s.find(n) != std::string::npos; }

int main() {
    const dect2::ModeTuning t = dect2::nfcTuning();
    CHECK(t.stdMode == 150, "stdMode %d, want 150", t.stdMode);
    CHECK(std::string(t.id) == "nfc", "id '%s'", t.id);
    CHECK(t.minMhz > 13.5 && t.maxMhz < 13.6 && t.defMhz > 13.55 && t.defMhz < 13.57, "13.56 MHz band");
    CHECK(t.minSampleRate >= 2000000, "min sample rate %.0f covers the subcarrier", t.minSampleRate);
    CHECK(t.carrierAtCentre, "carrierAtCentre must be set (the carrier is the wanted signal)");
    const dect2::ModeTuning* reg = dect2::modeTuning(150);
    CHECK(reg != nullptr, "modeTuning(150) not registered");
    if (reg) CHECK(std::string(reg->name) == "NFC / RFID 13.56 MHz", "registered name '%s'", reg->name);

    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(has(table, "{148, \"NFC / RFID 13.56 MHz\","), "mode row for family 148 missing");

    const std::string appH = readFile("app/app.h");
    const size_t p = appH.find("kNumFamilies = ");
    int fam = 0;
    if (p != std::string::npos) fam = std::atoi(appH.c_str() + p + 15);
    CHECK(fam > 148, "kNumFamilies (%d) must exceed the NFC family 148", fam);

    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(has(modeUi, "case 148: return &kNfcUi;"), "family 148 not mapped to kNfcUi");

    const std::string cmake = readFile("CMakeLists.txt");
    CHECK(has(cmake, " nfc "), "nfc not in DECT2_MODES");
    size_t lists = 0;
    for (size_t q = cmake.find("set(DECT2_MODES"); q != std::string::npos; q = cmake.find("set(DECT2_MODES", q + 1)) lists++;
    CHECK(lists == 1, "%d DECT2_MODES lists, want exactly 1", (int)lists);

    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(has(tel, "case 149: return pollStd(rxNfc_, 149,"), "telemetry case 149 missing");
    const std::string eng = readFile("core/src/engine/engine.cpp");
    CHECK(has(eng, "a == 149) rxNfc_.feed"), "feedReceiver case 149 missing");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "case 150: return nfc::makeNfcSynth"), "makeModeSynth case 150 missing");
    CHECK(has(modes, "case 149: return nfcSummary"), "modeSummary case 149 missing");
    CHECK(has(modes, "nfcTuning()"), "nfcTuning not in the mode table");

    const std::string ui = readFile("app/nfc/nfc_ui.cpp");
    const char* strings[] = {"NFC / RFID 13.56 MHz", "a.rx.standard == 149", "Frame log", "Tags", "NDEF", "receive only"};
    for (const char* s : strings) CHECK(has(ui, s), "panel string missing: %s", s);

    const std::string tx = readFile("core/src/tx/tx.cpp");
    CHECK(has(tx, "makeNfcReaderTxGenerator"), "the active reader is not wired into the TX table");

    std::printf("%s: %d failure(s)\n", fails ? "test_nfc_mode FAILED" : "test_nfc_mode OK", fails);
    return fails ? 1 : 0;
}
