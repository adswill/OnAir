// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Flipper Zero Sub-GHz chat wiring: the tuning under its engine standard, the mode row, family and poll-slot counts, the engine cases, the panel
// hook, the TX row (finite, one line of text), and that the screen keeps the transmit rules (the notice, the claim, the amplifier off, no saved key).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/esubghz/esubghz_rx.h"
#include "dect2/tx/tx.h"
#include "../common/test_util.h"

static std::string readFile(const std::string& rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

int main() {
    const dect2::ModeTuning mt = dect2::esubghzTuning();
    CHECK(mt.stdMode == 206 && dect2::modeTuningById("esubghz") && dect2::modeTuningById("esubghz")->stdMode == 206 && dect2::modeTuning(206), "tuning table");
    CHECK(mt.defMhz > 433.919 && mt.defMhz < 433.921 && mt.tuneOffsetHz > 0, "433.92 MHz, off the DC spike");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "{204, \"Flipper Zero Sub-GHz chat\", 8,"), "mode row");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 204: return &kEsubghzUi;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 205, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 166, "kPollSeqSlots"); }
    CHECK(has(readFile("core/src/engine/engine_telemetry.cpp"), "case 205: return pollStd(rxEsg_, 205, t, seqs[165]"), "telemetry case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "esubghzTuning()") && has(modes, "case 206: return makeEsubghzSynth") && has(modes, "case 205: return esubghzSummary"), "modes.cpp wiring");
    const dect2::TxModeSpec* m = dect2::txModeById("esubghz");
    CHECK(m && m->stdMode == 206 && dect2::txModeFinite(*m) && dect2::txPayloadKind("esubghz") == dect2::TxPayloadKind::Keyboard, "TX row");
    CHECK(dect2::txModeFor(206) == m, "TX row by engine standard");
    const std::string ui = readFile("app/esubghz/esubghz_ui.cpp");
    CHECK(has(ui, "txNoticeAccepted") && has(ui, "txClaimRadio") && has(ui, "txReleaseRadio") && has(ui, "r.tune.ampOn = false") && has(ui, "mapPauseForTransmit"), "the screen keeps the transmit rules");
    CHECK(!has(ui, "setS(\"esgPassword") && !has(ui, "setS(\"esgHex"), "the password and key are never saved");
    CHECK(has(readFile("docs/esubghz.md"), "Half duplex"), "the help page explains half duplex");
    return testSummary("esubghz mode");
}
