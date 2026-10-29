// Wi-Fi HaLow / 802.11ah through the engine (setStandard(72)): the synthetic source plays the test schedule, the receiver reports through latestRx().
// No sound.
#include "dect2/halow/halow_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = halowTuning();
    CHECK(mt.stdMode == 72 && modeTuningById("halow") && modeTuningById("halow")->stdMode == 72, "tuning table");
    CHECK(minSampleRateFor(72, 4) == 2e6, "minimum rate");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 72;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(72);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 71, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    // 5 s, and on a slow or busy machine (the synthetic source then makes less signal in that time) on until the results below are in,
    // at most 20 s
    const auto done = [&] { return rx.seq > 10 && rx.halow.nets.size() == 4 && rx.halow.blocksOk > 300 && rx.halow.ppsHist.size() >= 3; };
    const auto t0 = std::chrono::steady_clock::now();
    for (double el = 0; el < 5.0 || (el < 20.0 && !done()); el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 71) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const HalowTelemetry& d = rx.halow;
    printf("  %d reports: %s, %zu networks, %llu bad\n", reports, halowSummary(d).c_str(), d.nets.size(), (unsigned long long)d.blocksBad);
    // reports made by the engine (its sequence number), not the polls that happened to see one
    CHECK(rx.seq > 10 && wrong == 0, "%llu reports made (%d seen), %d wrong", (unsigned long long)rx.seq, reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.nets.size() == 4 && d.blocksOk > 300 && d.blocksBad == 0, "%zu networks, %llu frames", d.nets.size(), (unsigned long long)d.blocksOk);
    CHECK(d.centerMhz == 863.5 && d.chanHint == 1, "channel hint %d at %.1f MHz", d.chanHint, d.centerMhz);
    CHECK(d.ppsHist.size() >= 3 && d.pps > 20, "pps %.0f, %zu history", d.pps, d.ppsHist.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.halow().clearResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    e.stop();
    {   // the same through the engine at 20 Msps: the 8 and 16 MHz networks join (the app picks 20 Msps for Auto, all widths)
        Engine e2;
        TuneSettings t2 = t;
        t2.sampleRate = 20e6;
        t2.bandwidthMhz = 16;
        e2.setStandard(72);
        CHECK(e2.start(dev, t2, fo), "engine start at 20 Msps");
        RxTelemetry rx2;
        uint64_t seq2 = 0;
        const auto s0 = std::chrono::steady_clock::now();
        const auto all = [&] { return rx2.halow.nets.size() == 6 && rx2.halow.perWidth[3] > 0 && rx2.halow.perWidth[4] > 0; };
        for (double el = 0; el < 6.0 || (el < 20.0 && !all()); el = std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count()) {   // as above
            if (e2.latestRx(rx2, seq2)) seq2 = rx2.seq;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const HalowTelemetry& d2 = rx2.halow;
        printf("  20 Msps: %s, %zu networks, widths 8 / 16 MHz: %llu / %llu\n", halowSummary(d2).c_str(), d2.nets.size(), (unsigned long long)d2.perWidth[3], (unsigned long long)d2.perWidth[4]);
        CHECK(d2.nets.size() == 6 && d2.blocksBad == 0 && d2.perWidth[3] > 0 && d2.perWidth[4] > 0 && d2.maxWidthMhz == 16, "%zu networks, %llu bad, max %d MHz", d2.nets.size(), (unsigned long long)d2.blocksBad, d2.maxWidthMhz);
        CHECK(e2.droppedSamples() == 0, "dropped %llu at 20 Msps", (unsigned long long)e2.droppedSamples());
        e2.stop();
    }
    printf(fails ? "halow engine: %d FAILED\n" : "halow engine: all passed\n", fails);
    return fails ? 1 : 0;
}
