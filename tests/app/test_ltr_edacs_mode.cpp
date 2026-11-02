// LTR and EDACS trunking modes (work in progress): the engine tunings are registered under their numbers, the mode rows, the family count and the
// screen mapping agree, and the panel strings are in the screen sources.
#include "dect2/edacs/edacs_rx.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/ltr/ltr_rx.h"
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
    CHECK(dect2::ltrTuning().stdMode == 131 && dect2::edacsTuning().stdMode == 132, "std modes");
    const dect2::ModeTuning* l = dect2::modeTuning(131);
    const dect2::ModeTuning* e = dect2::modeTuning(132);
    CHECK(l && std::string(l->name) == "LTR trunking", "LTR not registered");
    CHECK(e && std::string(e->name) == "EDACS trunking", "EDACS not registered");
    CHECK(dect2::modeTuningById("ltr") != nullptr && dect2::modeTuningById("edacs") != nullptr, "ids not registered");

    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(table.find("{129, \"LTR trunking\", 5,") != std::string::npos, "mode row for family 129 missing");
    CHECK(table.find("{130, \"EDACS trunking\", 5,") != std::string::npos, "mode row for family 130 missing");
    const std::string appH = readFile("app/app.h");
    { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(appH.c_str() + q + 29) >= 131, "kNumFamilies does not cover family 130"); }
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(modeUi.find("case 129: return &kLtrUi;") != std::string::npos, "family 129 not mapped");
    CHECK(modeUi.find("case 130: return &kEdacsUi;") != std::string::npos, "family 130 not mapped");
    const std::string cm = readFile("CMakeLists.txt");
    size_t lists = 0;
    for (size_t p = cm.find("set(DECT2_MODES"); p != std::string::npos; p = cm.find("set(DECT2_MODES", p + 1)) lists++;
    CHECK(lists == 1, "%zu set(DECT2_MODES lines", lists);
    CHECK(cm.find(" ltr edacs ") != std::string::npos, "ltr edacs not in DECT2_MODES");

    const std::string lu = readFile("app/ltr/ltr_ui.cpp"), eu = readFile("app/edacs/edacs_ui.cpp");
    for (const char* s : {"LTR trunking", "Lock: %s", "Good words: %llu", "Repeaters", "Area: %d"})
        CHECK(lu.find(s) != std::string::npos, "LTR panel string missing: %s", s);
    for (const char* s : {"EDACS trunking", "Lock: %s", "Good frames: %llu", "Data words (28 bits, raw)", "Extended Addressing"})
        CHECK(eu.find(s) != std::string::npos, "EDACS panel string missing: %s", s);

    std::printf("%s: %d failure(s)\n", fails ? "test_ltr_edacs_mode FAILED" : "test_ltr_edacs_mode OK", fails);
    return fails ? 1 : 0;
}
