// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa (raw) through the engine (setStandard(73)): the synthetic source plays the test schedule, the receiver reports through latestRx(). No sound.
#include "dect2/loraraw/loraraw_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = loraRawTuning();
    CHECK(mt.stdMode == 73 && modeTuningById("loraraw") && modeTuningById("loraraw")->stdMode == 73, "tuning table");
    CHECK(mt.defMhz > 868 && mt.defMhz < 869 && mt.minMhz <= 433 && mt.maxMhz >= 928, "ISM range 433 / 868 / 915 MHz");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 73;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(73);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 72, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 12.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 72) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const LoraRawTelemetry& d = rx.loraraw;
    printf("  %d reports: %s\n", reports, loraRawSummary(d).c_str());
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.blocksOk >= 9 && d.blocksBad == 0 && !d.log.empty(), "%llu frames, %llu bad", (unsigned long long)d.blocksOk, (unsigned long long)d.blocksBad);
    CHECK(d.log.empty() || (!d.log[0].hex.empty() && !d.log[0].ascii.empty()), "hex / text");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.loraraw().clearResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    e.stop();
    printf(fails ? "loraraw engine: %d FAILED\n" : "loraraw engine: all passed\n", fails);
    return fails ? 1 : 0;
}
