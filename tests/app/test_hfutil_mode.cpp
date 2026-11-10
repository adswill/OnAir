// HF utility modes: registered tunings, the mode rows and the engine / panel wiring.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/hfutil/hfutil_rx.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)
static std::string readFile(const std::string& rel) { std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

int main() {
    for (int k = 0; k < dect2::kHfuKinds; k++) {
        const dect2::ModeTuning t = dect2::hfUtilTuning(k);
        CHECK(t.stdMode == dect2::kHfuFamilyFirst + k + 2, "engine standard of kind %d", k);
        CHECK(dect2::modeTuning(t.stdMode) != nullptr, "registered %d", t.stdMode);
    }
    const std::string tbl = readFile("app/ui/mode_table.cpp");
    for (const char* row : {"{430, \"MIL-STD-188-110 serial\"", "{431, \"STANAG 4285\"", "{432, \"ARQ-E\"", "{433, \"ARQ-N\"", "{434, \"AUTOSPEC\"", "{435, \"SPREAD\"", "{436, \"G-TOR\"", "{437, \"EFR teleswitch\"", "{438, \"Piccolo Mk VI\"", "{439, \"Coquelet-8\""})
        CHECK(has(tbl, row), "mode row %s", row);
    const std::string mu = readFile("app/panels/modeui.cpp");
    CHECK(has(mu, "return &kHfuUi[family - 430];"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 440, "kNumFamilies"); }
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(has(tel, "rxHfu_[a - 431]") && has(tel, "RxTelemetry::hfutil"), "telemetry poll");
    if (fails) std::printf("%d FAILED\n", fails); else std::printf("OK\n");
    return fails ? 1 : 0;
}
