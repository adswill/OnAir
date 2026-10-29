// Zigbee / IEEE 802.15.4 through the engine (setStandard(67)): the synthetic source plays the test schedule, the receiver reports through latestRx().
// No sound.
#include "dect2/zigbee/zigbee_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = zigbeeTuning();
    CHECK(mt.stdMode == 67 && modeTuningById("zigbee") && modeTuningById("zigbee")->stdMode == 67, "tuning table");
    CHECK(minSampleRateFor(67, 2) == 4e6, "minimum rate");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 67;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(67);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 66, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    // 5 s, and on a slow or busy machine (the synthetic source then makes less signal in that time) on until the results below are in,
    // at most 20 s
    const auto done = [&] { return rx.seq > 10 && rx.zigbee.pans.size() == 2 && rx.zigbee.devices.size() >= 5 && rx.zigbee.blocksOk > 300 && rx.zigbee.ppsHist.size() >= 3; };
    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    while (elapsed() < 5.0 || (elapsed() < 20.0 && !done())) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 66) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const ZigbeeTelemetry& d = rx.zigbee;
    printf("  %d reports: %s, %zu PANs, %zu devices, %llu bad\n", reports, zigbeeSummary(d).c_str(), d.pans.size(), d.devices.size(), (unsigned long long)d.blocksBad);
    // reports made by the engine (its sequence number), not the polls that happened to see one
    CHECK(rx.seq > 10 && wrong == 0, "%llu reports made (%d seen), %d wrong", (unsigned long long)rx.seq, reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.pans.size() == 2 && d.devices.size() >= 5 && d.blocksOk > 300 && d.blocksBad == 0, "%zu PANs, %zu devices, %llu frames", d.pans.size(), d.devices.size(), (unsigned long long)d.blocksOk);
    CHECK(d.channel == 11, "channel %d", d.channel);
    CHECK(d.ppsHist.size() >= 3 && d.pps > 20, "pps %.0f, %zu history", d.pps, d.ppsHist.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.zigbee().clearResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    e.stop();
    printf(fails ? "zigbee engine: %d FAILED\n" : "zigbee engine: all passed\n", fails);
    return fails ? 1 : 0;
}
