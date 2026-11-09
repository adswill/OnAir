// Inmarsat BGAN wiring: tuning, mode row, engine and panel wiring, the honesty strings, MODE-NUMBERS row.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/bgan/bgan_rx.h"
#include "dect2/engine/mode_tuning.h"
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)
static std::string readFile(const std::string& rel) { std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }
int main() {
    const dect2::ModeTuning mt = dect2::bganTuning();
    CHECK(mt.stdMode == 247 && dect2::modeTuningById("bgan") && dect2::modeTuningById("bgan")->stdMode == 247 && dect2::modeTuning(247), "tuning table");
    const std::string tbl = readFile("app/ui/mode_table.cpp");
    CHECK(has(tbl, "{245, \"Inmarsat BGAN\", 4,") && has(tbl, "Needs a user test") && has(tbl, "no subscriber identity"), "mode row names the limits");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 245: return &kBganUi;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 246, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 206, "kPollSeqSlots"); }
    CHECK(has(readFile("core/src/engine/engine_telemetry.cpp"), "case 246: return pollStd(rxBgan_, 246, t, seqs[205]"), "telemetry case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "bganTuning()") && has(modes, "case 247: return makeBganSynth") && has(modes, "case 246: return bganSummary"), "modes.cpp wiring");
    CHECK(has(readFile("docs/MODE-NUMBERS.md"), "| Inmarsat BGAN | 245 | 247 | 246 | 205 |"), "docs/MODE-NUMBERS.md row");
    std::printf(fails ? "bgan mode: %d FAILED\n" : "bgan mode: all passed\n", fails);
    return fails ? 1 : 0;
}
