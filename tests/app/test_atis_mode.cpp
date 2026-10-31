// nRF24L01 wiring: the tuning is registered under its engine standard, the mode row, family and poll-slot counts, the engine cases, the panel hook,
// the doc page and the panel / mode row say which radios work.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/atis/atis_rx.h"

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
    const dect2::ModeTuning mt = dect2::atisTuning();
    CHECK(mt.stdMode == 213 && dect2::modeTuningById("atis") && dect2::modeTuningById("atis")->stdMode == 213 && dect2::modeTuning(213), "tuning table");
    CHECK(mt.minMhz <= 156.3 && mt.maxMhz >= 162.0 && mt.minSampleRate >= 250e3 && mt.defMhz >= 156.3, "inland VHF channels");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "{211, \"ATIS (inland waterways)\""), "mode row");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 211: return &kAtisUi;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 212, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 188, "kPollSeqSlots"); }
    CHECK(has(readFile("core/src/engine/engine_telemetry.cpp"), "case 212: return pollStd(rxAtis_, 212, t, seqs[187]"), "telemetry case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "atisTuning()") && has(modes, "case 213: return makeAtisSynth") && has(modes, "case 212: return atisSummary"), "modes.cpp wiring");
    const std::string doc = readFile("docs/atis.md");
    CHECK(has(doc, "ETSI EN 300 698") && has(doc, "1300") && has(doc, "2100") && has(doc, "not AIS") && has(doc, "9 + "), "the help page names the format");
    const std::string ui = readFile("app/atis/atis_ui.cpp");
    CHECK(has(ui, "not AIS") && has(ui, "ETSI EN 300 698"), "the panel explains ATIS");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "Automatic Transmitter Identification System"), "mode row says what ATIS is");
    CHECK(has(readFile("docs/MODE-NUMBERS.md"), "| ATIS (inland waterways) | 211 | 213 | 212 | 187 |"), "docs/MODE-NUMBERS.md row");
    std::printf(fails ? "atis mode: %d FAILED\n" : "atis mode: all passed\n", fails);
    return fails ? 1 : 0;
}
