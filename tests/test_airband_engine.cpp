// Airband through the engine (setStandard(28)): the synthetic source plays the test layout, the receiver hears its channels and reports
// through latestRx(); a retune keeps the report numbers growing.
#include "dect2/airband_rx.h"
#include "dect2/engine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const ModeTuning mt = airbandTuning();
    CHECK(mt.stdMode == 28 && modeTuningById("airband") && modeTuningById("airband")->stdMode == 28, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 28;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.airband().setSilent(true);
    e.setStandard(28);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 27, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 9.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 27) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const AirbandTelemetry& a = rx.airband;
    printf("  %d reports: %s, %zu log entries, centre %.4f MHz\n", reports, airbandSummary(a).c_str(), a.activity.size(), a.centerHz / 1e6);
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(std::fabs(a.centerHz - (t.centerHz + mt.tuneOffsetHz)) < 1, "centre %.0f", a.centerHz);
    int inBand = 0;
    for (const auto& c : a.channels) inBand += c.inBand;
    CHECK(a.channels.size() == 5 && inBand == 5, "%zu channels, %d in band", a.channels.size(), inBand);
    CHECK(a.activity.size() >= 4 && rx.blocksOk == a.blocksOk && a.blocksOk >= 4, "%zu transmissions", a.activity.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    const uint64_t before = rx.seq;
    TuneSettings t2 = t; t2.centerHz = 119.1e6;
    e.retuneReset(t2);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    e.latestRx(rx, 0);
    CHECK(rx.seq > before && std::fabs(rx.airband.centerHz - (119.1e6 + mt.tuneOffsetHz)) < 1, "after retune: seq %llu, centre %.0f", (unsigned long long)rx.seq, rx.airband.centerHz);
    e.stop();
    printf(fails ? "airband engine: %d FAILED\n" : "airband engine: all passed\n", fails);
    return fails ? 1 : 0;
}
