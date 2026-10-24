// P25 Phase 2 mode: the engine tuning is registered under its number (family 180, engine standard 182), the mode row, the family count, the poll slot and the
// screen mapping agree, and the panel strings are in the screen source.
#include "dect2/engine/mode_tuning.h"
#include "dect2/p25p2/p25p2_rx.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static std::string readFile(const std::string& rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    CHECK(dect2::p25p2Tuning().stdMode == 182, "std mode");
    const dect2::ModeTuning* p = dect2::modeTuning(182);
    CHECK(p && std::string(p->name) == "P25 Phase 2", "P25 Phase 2 not registered");
    CHECK(dect2::modeTuningById("p25p2") != nullptr, "id not registered");
    CHECK(dect2::modeTuning(52) && std::string(dect2::modeTuning(52)->id) == "p25", "Phase 1 tuning disturbed");

    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(table.find("{180, \"P25 Phase 2\", 5,") != std::string::npos, "mode row for family 180 missing");
    const std::string appH = readFile("app/app.h");
    { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(appH.c_str() + q + 29) >= 181, "kNumFamilies does not cover family 180"); }
    const std::string eng = readFile("core/include/dect2/engine/engine.h");
    { const size_t q = eng.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(eng.c_str() + q + 16) >= 182, "kPollSeqSlots does not cover slot 181"); }
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(modeUi.find("case 180: return &kP25p2Ui;") != std::string::npos, "family 180 not mapped");
    const std::string cm = readFile("CMakeLists.txt");
    CHECK(cm.find(" p25p2 ") != std::string::npos, "p25p2 not in DECT2_MODES");
    CHECK(cm.find("test_p25p2_plugin") != std::string::npos, "the plug-in test is not given the stub codec path");
    const std::string ui = readFile("app/p25p2/p25p2_ui.cpp");
    for (const char* s : {"Listen to", "first heard", "slot 1", "slot 2", "a.rx.standard == 181", "WACN / system / NAC"})
        CHECK(ui.find(s) != std::string::npos, "P25 Phase 2 panel string missing: %s", s);
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(tel.find("case 181: return pollStd(rxP25p2_, 181, t, seqs[181]") != std::string::npos, "poll case missing");

    std::printf("%s: %d failure(s)\n", fails ? "test_p25p2_mode FAILED" : "test_p25p2_mode OK", fails);
    return fails ? 1 : 0;
}
