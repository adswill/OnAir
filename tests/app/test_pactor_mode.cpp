// PACTOR-I wiring: the tuning is registered under its engine standard, the mode row, family and poll-slot counts, the engine cases, the panel hook,
// the help page and the panel say "experimental" and that PACTOR-II / -III / -IV are not supported.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/pactor/pactor_rx.h"

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
    const dect2::ModeTuning mt = dect2::pactorTuning();
    CHECK(mt.stdMode == 205 && dect2::modeTuningById("pactor") && dect2::modeTuningById("pactor")->stdMode == 205 && dect2::modeTuning(205), "tuning table");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "{203, \"PACTOR-I (experimental)\", 12,"), "mode row");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 203: return &kPactorUi;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 204, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 165, "kPollSeqSlots"); }
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(has(tel, "case 204: return pollStd(rxPac_, 204, t, seqs[164]"), "telemetry case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "pactorTuning()") && has(modes, "case 205: return makePactorSynth") && has(modes, "case 204: return pactorSummary"), "modes.cpp wiring");
    const std::string doc = readFile("docs/pactor.md");
    CHECK(has(doc, "experimental") && has(doc, "PACTOR-II") && has(doc, "proprietary") && has(doc, "reverse engineer"), "the help page says experimental and names the unsupported modes");
    const std::string ui = readFile("app/pactor/pactor_ui.cpp");
    CHECK(has(ui, "Experimental") && has(ui, "not supported"), "the panel says experimental and names the limit");
    const std::string tbl = readFile("app/ui/mode_table.cpp");
    CHECK(has(tbl, "PACTOR-II, -III and -IV are proprietary"), "mode row names the limit");
    std::printf(fails ? "pactor mode: %d FAILED\n" : "pactor mode: all passed\n", fails);
    return fails ? 1 : 0;
}
