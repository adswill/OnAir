// GK-2A through the engine (setStandard(232) LRIT, 233 HRIT): the synthetic source plays the test signal, the receiver reports through latestRx().
#include "dect2/engine/engine.h"
#include "dect2/gk2a/gk2a_rx.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static void run(int std_, double seconds, int minPictures) {
    const ModeTuning* mt = modeTuning(std_);
    CHECK(mt != nullptr && mt->stdMode == std_, "tuning table %d", std_);
    if (!mt) return;
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt->defMhz * 1e6;
    t.bandwidthMhz = mt->bandwidthMhz;
    t.sampleRate = mt->sampleRate;
    t.synth.mode = std_;
    t.synth.snrDb = std_ == 233 ? 12 : 8;
    t.synth.cfoHz = 15e3;
    FileOptions fo;
    e.setStandard(std_);
    CHECK(e.start(dev, t, fo), "engine start %d", std_);
    CHECK(e.activeStandard() == std_ - 1, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != std_ - 1) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const Gk2aTelemetry& g = rx.gk2a;
    printf("  standard %d: %d reports: %s\n", std_, reports, gk2aSummary(g).c_str());
    CHECK(reports > 8 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(g.link == (std_ == 233 ? 1 : 0), "link");
    CHECK(g.blocksOk >= 10, "%llu frames", (unsigned long long)g.blocksOk);
    int pics = 0;
    for (const auto& p : g.products) { std::lock_guard<std::mutex> l(p->mu); if (p->complete && p->channelIdx >= 0 && p->rowsDone == p->h) pics++; }
    CHECK(pics >= minPictures, "%d pictures", pics);
    e.stop();
}

int main() {
    run(233, 8.0, 1);      // HRIT: frames 366 a second, the first round of files takes about 0.2 s
    run(232, 16.0, 1);     // LRIT: 64 kbit/s, two channels and the texts take about 4 s
    return testSummary("gk2a engine");
}
