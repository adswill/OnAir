// Mercury mode wiring: the engine tuning is registered under its number (family 204, active standard 205, engine standard 206,
// telemetry slot 165), the mode row, the family and slot counts, the panel and the TX studio entry agree with it, and the panel keeps
// its monitor-only strings.
#include "dect2/mercury/mercury_rx.h"
#include "dect2/engine/mode_tuning.h"
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
    const dect2::ModeTuning t = dect2::mercuryTuning();
    CHECK(t.stdMode == 206, "stdMode %d, want 206", t.stdMode);
    const dect2::ModeTuning* reg = dect2::modeTuning(206);
    CHECK(reg != nullptr && std::string(reg->id) == "mercury", "modeTuning(206) not registered");
    const std::string table = readFile("app/ui/mode_table.cpp");
    CHECK(table.find("{204, \"Mercury (HF OFDM modem)\", 10,") != std::string::npos, "mode row for family 204 missing");
    CHECK(table.find("Experimental: checked against its own modulator") != std::string::npos, "mode row must say experimental");
    const std::string appH = readFile("app/app.h");
    { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && std::atoi(appH.c_str() + q + 29) >= 205, "kNumFamilies does not cover family 204"); }
    const std::string modeUi = readFile("app/panels/modeui.cpp");
    CHECK(modeUi.find("case 204: return &kMercuryUi;") != std::string::npos, "family 204 not mapped to kMercuryUi");
    const std::string tel = readFile("core/src/engine/engine_telemetry.cpp");
    CHECK(tel.find("case 205: return pollStd(rxMercury_, 205, t, seqs[165]") != std::string::npos, "telemetry case 205 missing");
    const std::string eng = readFile("core/include/dect2/engine/engine.h");
    { const size_t q = eng.find("kPollSeqSlots = "); CHECK(q != std::string::npos && std::atoi(eng.c_str() + q + 16) >= 166, "kPollSeqSlots does not cover slot 165"); }
    const std::string feed = readFile("core/src/engine/engine.cpp");
    CHECK(feed.find("else if (a == 205) rxMercury_.feed(x, n);") != std::string::npos, "feed dispatch 205 missing");
    const std::string tx = readFile("core/src/tx/tx.cpp");
    CHECK(tx.find("mk(\"mercury\", \"Mercury (HF OFDM modem)\", 206, \"makeMercurySynth") != std::string::npos, "TX studio entry missing");
    const std::string ui = readFile("app/mercury/mercury_ui.cpp");
    const char* strings[] = {"encrypted: the payload is not decoded", "never opened or run", "Known callsigns", "Sessions", "Broadcast"};
    for (const char* s : strings) CHECK(ui.find(s) != std::string::npos, "panel string missing: %s", s);
    std::printf("%s: %d failure(s)\n", fails ? "test_mercury_mode FAILED" : "test_mercury_mode OK", fails);
    return fails ? 1 : 0;
}
