// FengYun geostationary modes: FY-4A LRIT (family 232), FY-4B LRIT (233), FY-4A HRIT-II/III (234), FY-2 S-VISSR (235). Active standard = family + 1,
// engine standard = family + 2, poll slot = family. The tunings are registered with a test signal each, the mode rows and the family count agree,
// the engine resets / configures / polls them, the poll slots are not shared, the screens carry their receive-only notes and the dish hint.
#include <cstdlib>
#include "dect2/fygeo/fygeo_rx.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include <cstdio>
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
static bool has(const std::string& hay, const std::string& n) { return hay.find(n) != std::string::npos; }

int main() {
    struct M { int fam; const char* id; const char* name; const char* ui; };
    const M ms[] = {{232, "fy4alrit", "FY-4A LRIT", "kFy4aLritUi"}, {233, "fy4blrit", "FY-4B LRIT", "kFy4bLritUi"},
                    {234, "fy4ahrit", "FY-4A HRIT-II/III", "kFy4aHritUi"}, {235, "fy2svissr", "FY-2 S-VISSR", "kFy2SvissrUi"}};
    const std::string table = readFile("app/ui/mode_table.cpp"), modeUi = readFile("app/panels/modeui.cpp"), tel = readFile("core/src/engine/engine_telemetry.cpp");
    const std::string appH = readFile("app/app.h"), engH = readFile("core/include/dect2/engine/engine.h"), place = readFile("core/src/engine/engine_place.cpp");
    const std::string eng = readFile("core/src/engine/engine.cpp"), ui = readFile("app/fygeo/fygeo_ui.cpp"), res = readFile("core/src/results/engine_results.cpp");
    for (const M& m : ms) {
        const int act = m.fam + 1, std = m.fam + 2;
        const dect2::ModeTuning* t = dect2::modeTuning(std);
        CHECK(t != nullptr, "modeTuning(%d) not registered", std);
        if (t) CHECK(std::string(t->id) == m.id && std::string(t->name) == m.name, "tuning %d is %s", std, t->id);
        const dect2::ModeTuning* byId = dect2::modeTuningById(m.id);
        CHECK(byId != nullptr && byId->stdMode == std, "modeTuningById(%s)", m.id);
        dect2::SynthConfig sc;
        CHECK(dect2::makeModeSynth(std, sc, m.fam == 234 ? 4000000 : 2400000) != nullptr, "%s: no test signal", m.id);
        CHECK(has(table, "{" + std::to_string(m.fam) + ", \"" + m.name + "\","), "mode row for family %d missing", m.fam);
        CHECK(has(modeUi, "case " + std::to_string(m.fam) + ": return &" + m.ui + ";"), "family %d not mapped to %s", m.fam, m.ui);
        CHECK(has(tel, "case " + std::to_string(act) + ": return pollStd(rxFy_, " + std::to_string(act) + ", t, seqs[" + std::to_string(m.fam) + "], &RxTelemetry::fygeo)"), "telemetry case %d missing", act);
        { const size_t q = appH.find("constexpr int kNumFamilies = "); CHECK(q != std::string::npos && atoi(appH.c_str() + q + 29) > m.fam, "kNumFamilies does not cover family %d", m.fam); }
        { const size_t q = engH.find("kPollSeqSlots = "); CHECK(q != std::string::npos && atoi(engH.c_str() + q + 16) > m.fam, "kPollSeqSlots does not cover slot %d", m.fam); }
        CHECK(has(res, std::string("ResultSlot{\"") + m.id + "\", " + std::to_string(act) + ", [this] { return rxFy_.saveResults("), "results slot for %d", act);
        CHECK(has(ui, m.ui), "%s not defined", m.ui);
        CHECK(t && t->minSampleRate > 0 && t->sampleRate >= t->minSampleRate && t->defMhz > 1600 && t->defMhz < 1720, "%s: tuning numbers", m.id);
    }
    CHECK(has(eng, "case 233: case 234: case 235: case 236: rxFy_.reset(); break;"), "engine reset");
    CHECK(has(eng, "else if (a >= 233 && a <= 236) rxFy_.feed(x, n);"), "engine feed");
    CHECK(has(place, "case 233: case 234: case 235: case 236: rxFy_.setLink(a - 233); rxFy_.configure(r); break;"), "engine configure");
    CHECK(has(place, "case 233: case 234: case 235: case 236: rxFy_.setLink(a - 233); rxFy_.setSignalOffset(o); break;"), "engine place");
    CHECK(has(ui, "Receive only") && has(ui, "dish"), "the screens say receive only and name the dish");
    CHECK(has(ui, "Why nothing decodes yet"), "the receiver tab shows the hint");
    CHECK(has(readFile("docs/MODE-NUMBERS.md"), "FY-4A LRIT") && has(readFile("docs/MODE-NUMBERS.md"), "FY-2 S-VISSR"), "docs/MODE-NUMBERS.md lists the modes");
    CHECK(has(readFile("THIRD_PARTY.md"), "OpenJPEG") && has(readFile("THIRD_PARTY.md"), "SatDump FengYun"), "THIRD_PARTY.md credits OpenJPEG and SatDump");
    CHECK(has(readFile("app/panels/credits_data.h"), "OpenJPEG"), "in-app credits");
    std::printf("%s: %d failure(s)\n", fails ? "test_fygeo_mode FAILED" : "test_fygeo_mode OK", fails);
    return fails ? 1 : 0;
}
