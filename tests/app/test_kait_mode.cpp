// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The KAIT mode (family 131, engine standard 133, active standard 132, telemetry slot 132) in the group "Kris custom modes": the engine tuning, the mode row, the
// panel strings, the engine wiring and the transmit registration are in the sources. Text checks only, so it builds without the app.
#include "dect2/kait/kait_live.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/tx/tx.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

static std::string readSource(const char* rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    int fails = 0;
    auto need = [&fails](const char* file, const std::string& src, const char* what) {
        if (src.find(what) == std::string::npos) {
            std::printf("FAIL: %s: missing %s\n", file, what);
            fails++;
        }
    };
    // the tuning is registered under its number
    const dect2::ModeTuning t = dect2::kaitTuning();
    if (t.stdMode != 133) { std::printf("FAIL: stdMode %d, want 133\n", t.stdMode); fails++; }
    const dect2::ModeTuning* reg = dect2::modeTuning(133);
    if (!reg || std::string(reg->id) != "kait") { std::printf("FAIL: modeTuning(133) is not kait\n"); fails++; }
    // the transmit registration
    const dect2::TxModeSpec* tx = dect2::txModeById("kait");
    if (!tx || tx->stdMode != 133) { std::printf("FAIL: KAIT is not a transmit mode with standard 133\n"); fails++; }
    else if (dect2::txPayloadKind("kait") != dect2::TxPayloadKind::Kait) { std::printf("FAIL: payload kind\n"); fails++; }

    const std::string table = readSource("app/ui/mode_table.cpp");
    need("app/ui/mode_table.cpp", table, "{131, \"KAIT\", 15,");
    const std::string app = readSource("app/app.h");
    { const size_t q = app.find("constexpr int kNumFamilies = "); if (q == std::string::npos || atoi(app.c_str() + q + 29) < 132) { std::printf("FAIL: kNumFamilies does not cover family 131\n"); fails++; } }
    const std::string modeui = readSource("app/panels/modeui.cpp");
    need("app/panels/modeui.cpp", modeui, "case 131: return &kKaitUi;");
    const std::string ui = readSource("app/kait/kait_ui.cpp");
    const char* strings[] = {"KAIT sends a 1080p picture in a 10 s burst", "Save picture...", "layers complete", "Picture file (PNG, PPM, JPEG ...)", "Callsign (9 characters at most)",
                             "a.rx.standard == 132"};
    for (const char* s : strings) need("app/kait/kait_ui.cpp", ui, s);
    const std::string eng = readSource("core/src/engine/engine.cpp");
    need("core/src/engine/engine.cpp", eng, "else if (a == 132) rxKait_.feed(x, n);");
    need("core/src/engine/engine.cpp", eng, "case 132: rxKait_.reset(); break;");
    need("core/src/engine/engine_place.cpp", readSource("core/src/engine/engine_place.cpp"), "case 132: rxKait_.configure(r); break;");
    need("core/src/engine/engine_start.cpp", readSource("core/src/engine/engine_start.cpp"), "rxKait_.configure(rate_);");
    need("core/src/engine/engine_telemetry.cpp", readSource("core/src/engine/engine_telemetry.cpp"), "case 132: return pollStd(rxKait_, 132, t, seqs[132], &RxTelemetry::kait);");
    need("core/src/engine/modes.cpp", readSource("core/src/engine/modes.cpp"), "case 132: return kaitSummary(t.kait);");
    need("core/src/engine/modes.cpp", readSource("core/src/engine/modes.cpp"), "kaitTuning(),");
    { const std::string e = readSource("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); if (q == std::string::npos || atoi(e.c_str() + q + 16) < 133) { std::printf("FAIL: kPollSeqSlots does not cover slot 132\n"); fails++; } }
    std::printf("%s: %d failure(s)\n", fails ? "test_kait_mode FAILED" : "test_kait_mode OK", fails);
    return fails ? 1 : 0;
}
