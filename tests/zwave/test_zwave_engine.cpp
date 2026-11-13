// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Z-Wave through the engine (setStandard(100)): the synthetic source plays the test network, the receiver reports through latestRx().
#include "dect2/zwave/zwave_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

int main() {
    const ModeTuning mt = zwaveTuning();
    CHECK(mt.stdMode == 100 && modeTuningById("zwave") && modeTuningById("zwave")->stdMode == 100, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 100;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(100);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 99, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 5.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 99) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const ZwaveTelemetry& d = rx.zwave;
    printf("  %d reports: %s, %llu bad\n", reports, zwaveSummary(d).c_str(), (unsigned long long)d.blocksBad);
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.region == "EU" && d.channelsCovered == 3, "region %s, %d channels", d.region.c_str(), d.channelsCovered);
    CHECK(d.networks.size() == 1 && d.nodes.size() >= 6 && d.blocksOk > 40 && d.blocksBad == 0, "%zu networks, %zu nodes, %llu frames", d.networks.size(), d.nodes.size(), (unsigned long long)d.blocksOk);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.zwave().clearResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    e.stop();
    if (fails) printf("zwave engine: %d FAILED\n", fails); else printf("zwave engine: all passed\n");
    return fails ? 1 : 0;
}
