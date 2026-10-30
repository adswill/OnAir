// nRF24L01 wiring: the tuning is registered under its engine standard, the mode row, family and poll-slot counts, the engine cases, the panel hook,
// the doc page and the panel / mode row say which radios work.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/nrf24/nrf24_rx.h"

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
    const dect2::ModeTuning mt = dect2::nrf24Tuning();
    CHECK(mt.stdMode == 212 && dect2::modeTuningById("nrf24") && dect2::modeTuningById("nrf24")->stdMode == 212 && dect2::modeTuning(212), "tuning table");
    CHECK(mt.minMhz == 2400 && mt.maxMhz >= 2525 && mt.minSampleRate >= 4e6, "2.4 GHz channels 0..125");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "{210, \"nRF24L01"), "mode row");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 210: return &kNrf24Ui;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 211, "kNumFamilies"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 187, "kPollSeqSlots"); }
    CHECK(has(readFile("core/src/engine/engine_telemetry.cpp"), "case 211: return pollStd(rxNrf_, 211, t, seqs[186]"), "telemetry case");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "nrf24Tuning()") && has(modes, "case 212: return makeNrf24Synth") && has(modes, "case 211: return nrf24Summary"), "modes.cpp wiring");
    const std::string doc = readFile("docs/nrf24.md");
    CHECK(has(doc, "Enhanced ShockBurst") && has(doc, "HackRF") && has(doc, "RTL-SDR") && has(doc, "CRC-8") && has(doc, "2400 + N"), "the help page names the radios and the format");
    const std::string ui = readFile("app/nrf24/nrf24_ui.cpp");
    CHECK(has(ui, "HackRF") && has(ui, "2.4 GHz"), "the panel says which radios work");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "2.4 GHz SDR"), "mode row names the radios");
    CHECK(has(readFile("docs/MODE-NUMBERS.md"), "| nRF24L01 | 210 | 212 | 211 | 186 |"), "docs/MODE-NUMBERS.md row");
    std::printf(fails ? "nrf24 mode: %d FAILED\n" : "nrf24 mode: all passed\n", fails);
    return fails ? 1 : 0;
}
