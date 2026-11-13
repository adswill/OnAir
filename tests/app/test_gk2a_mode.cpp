// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GK-2A wiring: the tunings are registered under their engine standards, the mode rows, family and poll-slot counts, the engine cases, the panel hooks,
// the doc page and the numbers table.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/gk2a/gk2a_rx.h"

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
    const dect2::ModeTuning l = dect2::gk2aLritTuning(), h = dect2::gk2aHritTuning();
    CHECK(l.stdMode == 232 && dect2::modeTuningById("gk2alrit") && dect2::modeTuningById("gk2alrit")->stdMode == 232 && dect2::modeTuning(232), "LRIT tuning table");
    CHECK(h.stdMode == 233 && dect2::modeTuningById("gk2ahrit") && dect2::modeTuningById("gk2ahrit")->stdMode == 233 && dect2::modeTuning(233), "HRIT tuning table");
    CHECK(std::abs(l.defMhz - 1692.14) < 1e-6 && std::abs(h.defMhz - 1695.4) < 1e-6, "frequencies of the KMA specifications");
    CHECK(l.minSampleRate >= 2.1 * 128e3 - 1 && l.minSampleRate <= 400e3, "LRIT minimum rate");
    CHECK(h.minSampleRate >= 2.1 * 3e6 && h.sampleRate >= h.minSampleRate && h.bandwidthMhz >= 4.5, "HRIT rates and bandwidth");
    const std::string tab = readFile("app/ui/mode_table.cpp");
    CHECK(has(tab, "{230, \"GK-2A LRIT\"") && has(tab, "{231, \"GK-2A HRIT\""), "mode rows");
    CHECK(has(tab, "1692.14 MHz") && has(tab, "1695.4 MHz") && has(tab, "DES") && has(tab, "none are built in"), "rows say what is needed");
    const std::string mu = readFile("app/panels/modeui.cpp");
    CHECK(has(mu, "case 230: return &kGk2aLritUi;") && has(mu, "case 231: return &kGk2aHritUi;"), "panel hooks");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 232, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 191, "kPollSeqSlots"); }
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(has(tel, "case 231: return pollStd(rxGkL_, 231, t, seqs[189]") && has(tel, "case 232: return pollStd(rxGkH_, 232, t, seqs[190]"), "telemetry cases");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "gk2aLritTuning()") && has(modes, "gk2aHritTuning()") && has(modes, "case 232: return makeGk2aLritSynth") && has(modes, "case 233: return makeGk2aHritSynth") && has(modes, "case 231: case 232: return gk2aSummary"), "modes.cpp wiring");
    const std::string doc = readFile("docs/GK2A.md");
    CHECK(has(doc, "1692.14") && has(doc, "1695.4") && has(doc, "QPSK") && has(doc, "DES") && has(doc, "JPEG 2000") && has(doc, "Es/N0") && has(doc, "SatDump") && has(doc, "unverified"), "the doc page");
    const std::string ui = readFile("app/gk2a/gk2a_ui.cpp");
    CHECK(has(ui, "Import keys") && has(ui, "EncryptionKeyMessage.bin") && has(ui, "kGk2aHritUi"), "the panel explains the keys");
    CHECK(has(readFile("docs/MODE-NUMBERS.md"), "| GK-2A LRIT | 230 | 232 | 231 | 189 |") && has(readFile("docs/MODE-NUMBERS.md"), "| GK-2A HRIT | 231 | 233 | 232 | 190 |"), "docs/MODE-NUMBERS.md rows");
    CHECK(has(readFile("THIRD_PARTY.md"), "GK-2A") && has(readFile("app/panels/credits_data.h"), "GK-2A"), "credits");
    std::printf(fails ? "gk2a mode: %d FAILED\n" : "gk2a mode: all passed\n", fails);
    return fails ? 1 : 0;
}
