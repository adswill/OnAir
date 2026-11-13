// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The SITOR mode is registered: a row in the mode table, a UI case in modeui.cpp, the family count covers it, and the
// panel carries the unverified-table warning. Reads the sources (DECT2_SOURCE_DIR), as test_credits does.
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "../common/test_util.h"

static std::string readSource(const char* rel) {
    std::ifstream f(std::string(DECT2_SOURCE_DIR) + "/" + rel, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main() {
    const std::string table = readSource("app/ui/mode_table.cpp");
    const std::string modeui = readSource("app/panels/modeui.cpp");
    const std::string app = readSource("app/app.h");
    const std::string panel = readSource("app/hfdig/hfdig_sitor_ui.cpp");
    CHECK(table.find("{119, \"SITOR\", 10,") != std::string::npos, "SITOR row (family 119) missing from the mode table");
    CHECK(table.find("SITOR (WIP") == std::string::npos, "SITOR still marked work in progress");
    CHECK(modeui.find("case 119: return &kSitorUi;") != std::string::npos, "family 119 has no UI in modeui.cpp");
    { const size_t q = app.find("kNumFamilies = "); CHECK(q != std::string::npos && atoi(app.c_str() + q + 15) >= 120, "kNumFamilies does not cover family 119"); }
    CHECK(panel.find("dect2::sitorSummary") != std::string::npos && panel.find("t.text.c_str()") != std::string::npos, "SITOR panel does not show the text");
    CHECK(readSource("core/src/engine/engine_telemetry.cpp").find("case 120: return pollStd(rxSitor_, 120") != std::string::npos, "SITOR is not polled");
    CHECK(readSource("core/src/engine/engine.cpp").find("else if (a == 120) rxSitor_.feed(x, n);") != std::string::npos, "SITOR is not fed");
    CHECK(readSource("core/src/engine/modes.cpp").find("sitorTuning()") != std::string::npos, "SITOR tuning is not registered");
    return testSummary("sitor_mode");
}
