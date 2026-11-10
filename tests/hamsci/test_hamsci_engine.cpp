// The ham science modes through the engine: setStandard(family + 2), the synthetic source plays the test signal, the receiver reports through latestRx().
// FSK441 (pings) and Throb2 (text). Simulated only.
#include "dect2/engine/engine.h"
#include "dect2/hamsci/hamsci_rx.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static RxTelemetry runKind(int kind, double seconds, int opt0, bool& ok) {
    const ModeTuning mt = hamsciTuning(kind);
    const int std_ = hamsciStd(kind);
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = std_;
    t.synth.snrDb = 25;
    t.synth.modeOpt[0] = opt0;
    FileOptions fo;
    e.setStandard(std_);
    ok = e.start(dev, t, fo);
    RxTelemetry rx, last;
    uint64_t seq = 0;
    int wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; last = rx; if (rx.standard != hamsciFamily(kind) + 1) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(wrong == 0, "kind %d: %d reports of the wrong standard", kind, wrong);
    CHECK(e.activeStandard() == hamsciFamily(kind) + 1, "active standard %d", e.activeStandard());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return last;
}

int main() {
    bool ok = false;
    RxTelemetry fsk = runKind(kHsFsk441, 28.0, 0, ok);
    CHECK(ok, "engine start (FSK441)");
    printf("  %s\n", hamsciSummary(fsk.hamsci).c_str());
    CHECK(fsk.rateOk && !fsk.hamsci.lines.empty() && fsk.hamsci.lines.front().text.find("K1JT") != std::string::npos, "FSK441 not decoded through the engine");
    RxTelemetry thr = runKind(kHsThrob, 38.0, 1, ok);
    CHECK(ok, "engine start (Throb)");
    printf("  %s\n  text: \"%s\"\n", hamsciSummary(thr.hamsci).c_str(), thr.hamsci.text.c_str());
    CHECK(thr.hamsci.text.find("CQ CQ DE W1AW") != std::string::npos || thr.hamsci.text.find("CQCQ") != std::string::npos, "Throb text not decoded through the engine");
    CHECK(thr.hamsci.variant == 1, "Throb variant %d", thr.hamsci.variant);
    return testSummary("hamsci engine");
}
