// PSK demodulator through the engine (setStandard(204)): the synthetic source plays the test signal at 2 Msps with the dial 250 kHz from
// the radio's centre, the receiver finds and locks it on its own and reports through latestRx(); the mode row, panel hook and poll slot
// are wired and the help says experimental.
#include "dect2/pskdsp/pskdsp_rx.h"
#include "dect2/pskdsp/pskdsp_tel.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static std::string readFile(const std::string& rel) {
    std::ifstream in(std::string(DECT2_SOURCE_DIR) + "/" + rel);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

int main() {
    const ModeTuning mt = pskdTuning();
    CHECK(mt.stdMode == 204 && modeTuningById("pskd") && modeTuningById("pskd")->stdMode == 204 && modeTuning(204), "tuning table");
    CHECK(has(readFile("app/ui/mode_table.cpp"), "{202, \"PSK demodulator (experimental)\", 4,"), "mode row");
    CHECK(has(readFile("app/panels/modeui.cpp"), "case 202: return &kPskdUi;"), "panel hook");
    CHECK(has(readFile("core/src/engine/engine_telemetry.cpp"), "case 203: return pollStd(rxPskd_, 203, t, seqs[163]"), "telemetry case");
    CHECK(has(readFile("docs/psk-demodulator.md"), "experimental"), "help page");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 204;
    t.synth.snrDb = 20;
    t.synth.modeOpt[0] = 1;       // QPSK
    t.synth.modeVal[0] = 4800;
    FileOptions fo;
    e.setStandard(204);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 203, "active standard %d", e.activeStandard());
    PskdSettings s; s.sync = "1ACFFC1D";
    e.pskd().setSettings(s);
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 12.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 203) wrong++; }
        if (rx.pskd.syncs >= 20) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const PskdTelemetry& p = rx.pskd;
    printf("  %d reports: %s, carrier %+.1f Hz, %llu sync words\n", reports, pskdSummary(p).c_str(), p.cfoHz, (unsigned long long)p.syncs);
    for (const auto& l : p.log) printf("    %s\n", l.c_str());
    CHECK(reports > 5 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(p.state == 2 && p.order == 2 && std::fabs(p.baud - 4800) < 10, "locked %d %s %.1f", p.state, pskdOrderName(p.order), p.baud);
    CHECK(p.syncs >= 10, "%llu sync words", (unsigned long long)p.syncs);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("pskdsp engine");
}
