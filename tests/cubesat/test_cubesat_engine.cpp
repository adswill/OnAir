// CubeSat telemetry through the engine (setStandard(63)): the synthetic source plays every framing, the receiver reports frames through
// latestRx(). No sound.
#include "dect2/cubesat/cubesat_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = cubesatTuning();
    CHECK(mt.stdMode == 63 && modeTuningById("cubesat") && modeTuningById("cubesat")->stdMode == 63, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 63;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(63);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 62, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    // 6 s, and on a slow or busy machine (the synthetic source then makes less signal in that time) on until the results below are in,
    // at most 20 s
    const auto done = [&] {
        int k = 0;
        for (int i = 0; i < cubesat::kNumFramings; i++) k += rx.cube.perFraming[i] > 0;
        return rx.seq > 10 && k == cubesat::kNumFramings;
    };
    const auto t0 = std::chrono::steady_clock::now();
    // a while with elapsed() instead of a for that updates the time in its step: Cppcheck called that for condition always false
    // (knownConditionTrueFalse), a false positive, and Codacy then fails the PR
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    while (elapsed() < 6.0 || (elapsed() < 20.0 && !done())) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 62) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const CubeTelemetry& c = rx.cube;
    printf("  %d reports: %s, %zu frames, %llu bad\n", reports, cubesatSummary(c).c_str(), c.frames.size(), (unsigned long long)c.blocksBad);
    // reports made by the engine (its sequence number), not the polls that happened to see one
    CHECK(rx.seq > 10 && wrong == 0, "%llu reports made (%d seen), %d wrong", (unsigned long long)rx.seq, reports, wrong);
    CHECK(rx.rateOk, "rate");
    int kinds = 0;
    for (int k = 0; k < cubesat::kNumFramings; k++) kinds += c.perFraming[k] > 0;
    CHECK(kinds == cubesat::kNumFramings, "framings heard: %d", kinds);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "cubesat engine: %d FAILED\n" : "cubesat engine: all passed\n", fails);
    return fails ? 1 : 0;
}
