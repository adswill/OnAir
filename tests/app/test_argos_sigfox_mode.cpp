// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Argos (family 152, active 153, engine standard 154, slot 143) and Sigfox (family 153, active 154, engine standard 155, slot 144): the tunings
// are registered under their numbers with a test signal each, the mode rows and the family count agree, the engine feeds and polls them,
// the poll slots are not shared, and the panels carry their receive-only notes.
#include <cstdlib>
#include "dect2/argos/argos_rx.h"
#include "dect2/sigfox/sigfox_rx.h"
#include "dect2/engine/mode_synth.h"
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
    struct M { int fam, act, std, slot; const char* id; const char* name; const char* ui; const char* rx; const char* tel; const char* file; };
    const M ms[] = {{152, 153, 154, 143, "argos", "Argos", "kArgosUi", "rxArgos_", "argos", "app/argos/argos_ui.cpp"},
                    {153, 154, 155, 144, "sigfox", "Sigfox", "kSigfoxUi", "rxSfx_", "sigfoxm", "app/sigfox/sigfox_ui.cpp"}};
    const std::string table = readFile("app/ui/mode_table.cpp"), modeUi = readFile("app/panels/modeui.cpp"), tel = readFile("core/src/engine/engine_telemetry.cpp");
    const std::string eng = readFile("core/src/engine/engine.cpp"), appH = readFile("app/app.h"), engH = readFile("core/include/dect2/engine/engine.h");
    for (const M& m : ms) {
        const dect2::ModeTuning* t = dect2::modeTuning(m.std);
        CHECK(t != nullptr, "modeTuning(%d) not registered", m.std);
        if (t) CHECK(std::string(t->id) == m.id && std::string(t->name) == m.name, "tuning %d is %s", m.std, t->id);
        dect2::SynthConfig sc;
        CHECK(dect2::makeModeSynth(m.std, sc, 256000) != nullptr, "%s: no test signal", m.id);
        CHECK(table.find("{" + std::to_string(m.fam) + ", \"" + m.name + "\",") != std::string::npos, "mode row for family %d missing", m.fam);
        CHECK(modeUi.find("case " + std::to_string(m.fam) + ": return &" + m.ui + ";") != std::string::npos, "family %d not mapped to %s", m.fam, m.ui);
        CHECK(tel.find("case " + std::to_string(m.act) + ": return pollStd(" + m.rx + ", " + std::to_string(m.act) + ", t, seqs[" + std::to_string(m.slot) + "], &RxTelemetry::" + m.tel + ")") != std::string::npos, "telemetry case %d missing", m.act);
        CHECK(eng.find("else if (a == " + std::to_string(m.act) + ") " + m.rx + ".feed(x, n);") != std::string::npos, "feed %d missing", m.act);
        { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(appH.c_str() + q + 29) > m.fam, "kNumFamilies does not cover family %d", m.fam); }
        { const size_t q = engH.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(engH.c_str() + q + 16) > m.slot, "kPollSeqSlots does not cover slot %d", m.slot); }
        CHECK(readFile(m.file).find("OnAir only receives") != std::string::npos, "%s: no receive-only note", m.file);
        const std::string k = "seqs[" + std::to_string(m.slot) + "]";
        size_t n = 0;
        for (size_t p = tel.find(k); p != std::string::npos; p = tel.find(k, p + 1)) n++;
        CHECK(n == 1, "slot %d used %zu times", m.slot, n);
    }
    CHECK(readFile("app/sigfox/sigfox_ui.cpp").find("never checked") != std::string::npos, "the Sigfox panel must say the authentication code is never checked");
    CHECK(readFile("app/argos/argos_ui.cpp").find("not public") != std::string::npos, "the Argos panel must say which parts are not public");
    std::printf("%s: %d failure(s)\n", fails ? "test_argos_sigfox_mode FAILED" : "test_argos_sigfox_mode OK", fails);
    return fails ? 1 : 0;
}
