// SELCAL wiring: tuning registered, mode row, family and poll-slot counts, engine cases, panel hook, the doc page.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/selcal/selcal_rx.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)
static std::string readFile(const std::string& rel) { std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

int main() {
    const dect2::ModeTuning mt = dect2::selcalTuning();
    CHECK(mt.stdMode == 242 && dect2::modeTuningById("selcal") && dect2::modeTuningById("selcal")->stdMode == 242 && dect2::modeTuning(242), "tuning table");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "{240, \"SELCAL (aircraft tone call)\""), "mode row");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 240: return &kSelcalUi;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 241, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 201, "kPollSeqSlots"); }
    CHECK(has(readFile("core/src/engine/engine_telemetry.cpp"), "case 241: return pollStd(rxSelcal_, 241, t, seqs[200]"), "telemetry case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "selcalTuning()") && has(modes, "case 242: return makeSelcalSynth") && has(modes, "case 241: return selcalSummary"), "modes.cpp wiring");
    CHECK(has(readFile("docs/selcal.md"), "ARINC 714") && has(readFile("docs/selcal.md"), "no map entry"), "doc page");
    CHECK(has(readFile("docs/MODE-NUMBERS.md"), "| SELCAL (aircraft tone call) | 240 | 242 | 241 | 200 |"), "docs/MODE-NUMBERS.md row");
    std::printf(fails ? "selcal mode: %d FAILED\n" : "selcal mode: all passed\n", fails);
    return fails ? 1 : 0;
}
