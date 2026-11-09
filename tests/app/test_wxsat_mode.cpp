// The wxsat links: each has its own mode-list row, its tuning, poll slot and screen wiring.
#include "dect2/engine/mode_tuning.h"
#include "dect2/wxsat/wxsat_tel.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)
static std::string readFile(const char* rel) { std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }
static bool has(const std::string& s, const std::string& t) { return s.find(t) != std::string::npos; }
int main() {
    using namespace dect2;
    const std::string tbl = readFile("app/ui/mode_table.cpp"), tel = readFile("core/src/engine/engine_telemetry.cpp"), docs = readFile("docs/MODE-NUMBERS.md"), modes = readFile("core/src/engine/modes.cpp");
    for (int p = 0; p < kWxNumProfiles; p++) {
        const WxProfileInfo& i = wxProfile(p);
        const ModeTuning* t = modeTuningById(i.id);
        CHECK(t && t->stdMode == i.family + 2, i.id);
        CHECK(has(tbl, "{" + std::to_string(i.family) + ", \"" + i.name + "\""), i.name);
        CHECK(has(tel, "case " + std::to_string(i.family + 1) + ": return pollStd(rxWx_[" + std::to_string(p) + "]"), "poll slot");
        CHECK(has(docs, std::string("| ") + i.name + " | " + std::to_string(i.family) + " |"), "numbers doc");
    }
    CHECK(has(tbl, "stopped transmitting in 2025"), "retired satellites are named in the description");
    CHECK(has(readFile("THIRD_PARTY.md"), "SatDump (weather satellite plug-ins"), "THIRD_PARTY entry");
    CHECK(has(readFile("app/panels/credits_data.h"), "SatDump (weather plug-ins)"), "in-app credit");
    printf(fails ? "wxsat mode: %d FAILED\n" : "wxsat mode: all passed\n", fails);
    return fails ? 1 : 0;
}
