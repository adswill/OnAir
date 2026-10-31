// Shortwave wiring: the tuning is registered under its engine standard, the mode row (in the HF group), the family and poll-slot counts, the engine
// cases, the panel hook, the General screen sharing, the doc page and the decoder buttons: every decoder in the table has a receiver behind it.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "dect2/engine/mode_tuning.h"
#include "dect2/shortwave/shortwave_rx.h"
#include "dect2/shortwave/shortwave_route.h"

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
    const dect2::ModeTuning mt = dect2::shortwaveTuning();
    CHECK(mt.stdMode == 213 && dect2::modeTuningById("shortwave") && dect2::modeTuningById("shortwave")->stdMode == 213 && dect2::modeTuning(213), "tuning table");
    CHECK(mt.minMhz <= 0.2 && mt.maxMhz >= 30 && mt.minSampleRate <= 250000, "covers the HF bands, works from a narrow HF radio");
    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(has(table, "{211, \"Shortwave\", 12,"), "mode row in the HF UTILITY group");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 211: return &kShortwaveUi;"), "panel hook");
    { const std::string a = readFile("app/app.h"); const size_t q = a.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(a.c_str() + q + 29) >= 212, "kNumFamilies"); CHECK(has(a, "isGeneralFamily"), "General screen shared"); }
    { const std::string e = readFile("core/include/dect2/engine/engine.h"); const size_t q = e.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(e.c_str() + q + 16) >= 188, "kPollSeqSlots"); }
    CHECK(has(readFile("core/src/engine/engine_telemetry.cpp"), "case 212: return pollStd(rxSw_, 212, t, seqs[187]"), "telemetry case");
    const std::string eng = readFile("core/src/engine/engine.cpp");
    CHECK(has(eng, "a == 212) rxSw_.feed(x, n)") && has(eng, "case 212: rxSw_.reset();"), "engine feed and reset");
    CHECK(has(readFile("core/src/engine/engine_place.cpp"), "case 212: rxSw_.configure(r);") && has(readFile("core/src/engine/engine_place.cpp"), "case 212: rxSw_.setSignalOffset(o);"), "engine place");
    const std::string modes = readFile("core/src/engine/modes.cpp");
    CHECK(has(modes, "shortwaveTuning()") && has(modes, "case 213: return makeShortwaveSynth") && has(modes, "case 212: return shortwaveSummary"), "modes.cpp wiring");
    CHECK(has(readFile("docs/MODE-NUMBERS.md"), "| Shortwave | 211 | 213 | 212 | 187 |"), "docs/MODE-NUMBERS.md row");
    const std::string doc = readFile("docs/SHORTWAVE-MODE.md");
    CHECK(has(doc, "RTTY") && has(doc, "Codec2") && has(doc, "setDetected") && has(doc, "KiwiSDR") && has(doc, "Airspy HF+"), "the doc names the decoders, the hook and the radios");
    const std::string ui = readFile("app/shortwave/shortwave_ui.cpp");
    CHECK(has(ui, "Decoders available") && has(ui, "Auto") && has(ui, "Advanced"), "the screen has the strip, Auto and Advanced");
    // every decoder button has a receiver behind it (a case in makeSlot) and a typical frequency
    const std::string rx = readFile("core/src/shortwave/shortwave_rx.cpp");
    int n = 0;
    const dect2::swr::DecoderInfo* tab = dect2::swr::decoderTable(n);
    CHECK(n == dect2::swr::kDecCount && tab[0].defMhz > 0, "decoder table");
    CHECK(has(rx, "case Dec::Rtty") && has(rx, "case Dec::Cw") && has(rx, "case Dec::Psk") && has(rx, "case Dec::Js8") && has(rx, "case Dec::Ale") && has(rx, "case Dec::Ardop") &&
          has(rx, "case Dec::Pactor") && has(rx, "case Dec::Mt63") && has(rx, "case Dec::Fsq") && has(rx, "case Dec::Ifkp") && has(rx, "case Dec::Navtex") && has(rx, "case Dec::TimeSig") && has(rx, "case Dec::HfId"),
          "makeSlot covers the decoders");
    std::printf(fails ? "shortwave mode: %d FAILED\n" : "shortwave mode: all passed\n", fails);
    return fails ? 1 : 0;
}
