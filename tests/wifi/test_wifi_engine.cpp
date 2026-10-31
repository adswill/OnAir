// Wi-Fi networks through the engine (setStandard(56)): the synthetic source plays the test schedule, the receiver reports through
// latestRx(). No sound.
#include "dect2/wifi/wifi_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = wifiTuning();
    CHECK(mt.stdMode == 56 && modeTuningById("wifi") && modeTuningById("wifi")->stdMode == 56, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 56;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(56);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 55, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    // 5 s, and on a slow or busy machine (the synthetic source then makes less signal in that time) on until the results below are in,
    // at most 20 s
    const auto done = [&] { return rx.seq > 10 && rx.wifi.aps.size() == 3 && rx.wifi.beacons > 100; };
    const auto t0 = std::chrono::steady_clock::now();
    // a while with elapsed() instead of a for that updates the time in its step: Cppcheck called that for condition always false
    // (knownConditionTrueFalse), a false positive, and Codacy then fails the PR
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    while (elapsed() < 5.0 || (elapsed() < 20.0 && !done())) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 55) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const WifiTelemetry& d = rx.wifi;
    printf("  %d reports: %s, %llu frames, %llu bad\n", reports, wifiSummary(d).c_str(), (unsigned long long)d.blocksOk, (unsigned long long)d.blocksBad);
    // reports made by the engine (its sequence number), not the polls that happened to see one
    CHECK(rx.seq > 10 && wrong == 0, "%llu reports made (%d seen), %d wrong", (unsigned long long)rx.seq, reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.aps.size() == 3 && d.beacons > 100, "%zu networks, %llu beacons", d.aps.size(), (unsigned long long)d.beacons);
    CHECK(d.channel == 6, "channel %d", d.channel);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "wifi engine: %d FAILED\n" : "wifi engine: all passed\n", fails);
    return fails ? 1 : 0;
}
