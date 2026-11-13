// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISDB-S mode: the mode row, the family count, the screen mapping, the engine wiring and the panel's strings (text checks on the sources).
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "../common/test_util.h"

static std::string readFile(const std::string& rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(table.find("{154, \"ISDB-S (BS)\", 0,") != std::string::npos, "mode row for family 154 missing (group 0, with the TV modes)");
    const std::string appH = readFile("app/app.h");
    { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(appH.c_str() + q + 29) >= 155, "kNumFamilies does not cover family 154"); }
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(modeUi.find("case 154: return &kIsdbsUi;") != std::string::npos, "family 154 not mapped to kIsdbsUi");
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(tel.find("case 155: return pollStd(rxIsdbs_, 155,") != std::string::npos, "active standard 155 is not polled");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(modes.find("isdbsTuning()") != std::string::npos && modes.find("case 156: return makeIsdbsSynth(") != std::string::npos && modes.find("case 155: return isdbsSummary(") != std::string::npos, "modes.cpp wiring");
    const std::string eng = readFile("core/src/engine/engine.cpp");
    CHECK(eng.find("a == 155) rxIsdbs_.feed(x, n)") != std::string::npos && eng.find("case 155: rxIsdbs_.reset()") != std::string::npos, "engine feed / reset");
    const std::string start = readFile("core/src/engine/engine_start.cpp");
    CHECK(start.find("rxIsdbs_.setPacketCallback(") != std::string::npos, "the transport stream is not handed to the player");
    const std::string rx = readFile("core/src/isdbs/isdbs_rx.cpp");
    CHECK(rx.find("t.stdMode = 156;") != std::string::npos, "tuning standard 156");
    const std::string cmake = readFile("CMakeLists.txt");
    size_t lists = 0;
    for (size_t p = cmake.find("set(DECT2_MODES"); p != std::string::npos; p = cmake.find("set(DECT2_MODES", p + 1)) lists++;
    CHECK(lists == 1, "%zu set(DECT2_MODES lines (must be exactly one)", lists);
    CHECK(cmake.find(" isdbs)") != std::string::npos || cmake.find(" isdbs ") != std::string::npos, "isdbs missing from DECT2_MODES");
    const std::string ui = readFile("app/isdbs/isdbs_ui.cpp");
    const char* strings[] = {"BS channel", "Frame", "TMCC", "Es/N0", "Constellation (%zu symbols)", "RS ok/bad", "scrambled (conditional access) streams stay scrambled", "return a.rx.standard == 155;"};
    for (const char* s : strings) CHECK(ui.find(s) != std::string::npos, "panel string missing: %s", s);
    return testSummary("test_isdbs_mode");
}
