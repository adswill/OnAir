// EBICab / Eurobalise through the engine (setStandard(209)): the synthetic source plays bursts of balise telegrams, the receiver reports through latestRx().
#include "dect2/ebicab/ebicab_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = ebicabTuning();
    CHECK(mt.stdMode == 209 && modeTuningById("ebicab") && modeTuningById("ebicab")->stdMode == 209 && modeTuning(209), "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 209;
    t.synth.snrDb = 16;
    t.synth.modeOpt[0] = 2;
    FileOptions fo;
    e.setStandard(209);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 208, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 12.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 208) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const EbicabTelemetry& s = rx.ebicab;
    printf("  %d reports: %s\n", reports, ebicabSummary(s).c_str());
    CHECK(reports > 8 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(s.blocksOk >= 10 && !s.balises.empty(), "%llu telegrams, %zu balises", (unsigned long long)s.blocksOk, s.balises.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("ebicab engine");
}
