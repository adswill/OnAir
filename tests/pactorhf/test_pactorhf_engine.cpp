// PACTOR-II / -III / -IV through the engine (setStandard(239 .. 241)): the synthetic source plays the test signal, each receiver reports through latestRx().
#include "dect2/pactorhf/pactorhf_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

static void one(int variant) {
    const ModeTuning mt = pactorHfTuning(variant);
    const int std_ = variant == 2 ? 239 : variant == 3 ? 240 : 241;
    CHECK(mt.stdMode == std_ && modeTuning(std_) && modeTuningById(mt.id) && modeTuningById(mt.id)->stdMode == std_, "PACTOR-%d tuning table", variant);
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = 250000;
    t.synth.mode = std_;
    t.synth.snrDb = 25;
    t.synth.modeOpt[0] = 4;   // PACTOR-III speed level 4 (14 carriers, D-QPSK) or PACTOR-IV QPSK
    FileOptions fo;
    e.setStandard(std_);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == std_ - 1, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const double limit = variant == 4 ? 22.0 : 30.0;
    // the default run stops as soon as every check below already holds (the time limit stays); the long run plays the whole limit as before
    auto passed = [&] {
        const PactorHfTelemetry& q = rx.pactorhf;
        return reports > 10 && wrong == 0 && rx.rateOk && q.variant == variant && (variant != 2 || q.nCarriers == 2) &&
               (variant != 3 || (q.nCarriers == 14 && q.level == 4)) && (variant != 4 || q.constellation == 4) && e.droppedSamples() == 0;
    };
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < limit) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != std_ - 1) wrong++; }
        if (!testLong() && passed()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const PactorHfTelemetry& p = rx.pactorhf;
    printf("  PACTOR-%d: %d reports: %s\n", variant, reports, pactorHfSummary(p).c_str());
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(p.variant == variant, "found variant %d", p.variant);
    if (variant == 2) CHECK(p.nCarriers == 2, "carriers %d", p.nCarriers);
    if (variant == 3) CHECK(p.nCarriers == 14 && p.level == 4, "carriers %d level %d", p.nCarriers, p.level);
    if (variant == 4) CHECK(p.constellation == 4, "constellation %d", p.constellation);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
}

int main() {
    for (int v = 2; v <= 4; v++) one(v);
    return testSummary("pactorhf engine");
}
