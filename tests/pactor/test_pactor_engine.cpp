// PACTOR-I through the engine (setStandard(205)): the synthetic source plays the test signal, the receiver reports through latestRx().
#include "dect2/pactor/pactor_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = pactorTuning();
    CHECK(mt.stdMode == 205 && modeTuningById("pactor") && modeTuningById("pactor")->stdMode == 205 && modeTuning(205), "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 205;
    t.synth.snrDb = 20;
    FileOptions fo;
    e.setStandard(205);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 204, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 30.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 204) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const PactorTelemetry& p = rx.pactor;
    printf("  %d reports: %s\n  text: \"%s\"\n", reports, pactorSummary(p).c_str(), p.text.c_str());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(p.blocksOk >= 7 && p.csCount >= 5, "%llu packets, %llu CS", (unsigned long long)p.blocksOk, (unsigned long long)p.csCount);
    CHECK(p.text.find("LY2ABC de LY1XYZ") != std::string::npos, "text not decoded");
    CHECK(!p.stations.empty(), "stations");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("pactor engine");
}
