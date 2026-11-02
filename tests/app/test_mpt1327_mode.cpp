// MPT1327 trunking mode (work in progress): the mode row, the family count and the screen mapping in the app source, and the panel's strings.
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
    CHECK(table.find("{127, \"MPT1327 trunking\", 5,") != std::string::npos, "mode row for family 127 missing (group 5, with the other trunking modes)");
    const std::string appH = readFile("app/app.h");
    { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(appH.c_str() + q + 29) >= 128, "kNumFamilies does not cover family 127"); }
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(modeUi.find("case 127: return &kMpt1327Ui;") != std::string::npos, "family 127 not mapped to kMpt1327Ui");
    const std::string eng = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(eng.find("case 128: return pollStd(rxMpt_, 128,") != std::string::npos, "active standard 128 is not polled");
    const std::string cmake = readFile("CMakeLists.txt");
    size_t lists = 0;
    for (size_t p = cmake.find("set(DECT2_MODES"); p != std::string::npos; p = cmake.find("set(DECT2_MODES", p + 1)) lists++;
    CHECK(lists == 1, "%zu set(DECT2_MODES lines (must be exactly one)", lists);
    CHECK(cmake.find(" mpt1327 ") != std::string::npos, "mpt1327 missing from DECT2_MODES");
    const std::string ui = readFile("app/mpt1327/mpt1327_ui.cpp");
    const char* strings[] = {"MPT1327 trunking", "Lock: %s", "System identity: %d", "Codewords good: %llu, bad: %llu", "Channel table", "Caller", "Called", "Channel", "Time",
                             "tune to an MPT1327 control channel"};
    for (const char* s : strings) CHECK(ui.find(s) != std::string::npos, "panel string missing: %s", s);
    return testSummary("test_mpt1327_mode");
}
