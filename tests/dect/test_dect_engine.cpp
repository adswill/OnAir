// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DECT through the engine (setStandard(80)): the synthetic source plays the test base stations, the receiver reports through latestRx().
#include "dect2/engine/engine.h"
#include "dect2/dect/dect_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

// one run on a centre frequency; returns the telemetry (the synthetic source plays the test base stations)
static bool run(double carrierMhz, double seconds, DectTelemetry& out) {
    const ModeTuning mt = dectTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = carrierMhz * 1e6;   // the radio is tuned tuneOffsetHz above this, the receiver mixes it back
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 80;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(80);
    if (!e.start(dev, t, fo)) return false;
    RxTelemetry rx;
    uint64_t seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        if (e.latestRx(rx, seq)) seq = rx.seq;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    out = rx.dect;
    e.stop();
    return true;
}

int main() {
    {   // US DECT 6.0: the same bursts centred on 1921.536 MHz are found, on carrier 0 of the US band
        DectTelemetry d;
        CHECK(run(1921.536, 4.0, d), "us start");
        printf("  us: band %d carrier %d %.3f MHz, %zu bases\n", d.band, d.carrier, d.channelMhz, d.bases.size());
        CHECK(d.band == 1 && d.carrier == 0 && std::fabs(d.channelMhz - 1921.536) < 0.01, "us band %d carrier %d at %.3f", d.band, d.carrier, d.channelMhz);
        CHECK(d.bases.size() == 2 && d.blocksOk >= 50, "us: %zu bases, %llu frames", d.bases.size(), (unsigned long long)d.blocksOk);
        CHECK(run(1928.448, 3.0, d) && d.band == 1 && d.carrier == 4, "us carrier 4");
    }
    const ModeTuning mt = dectTuning();
    CHECK(mt.stdMode == 80 && modeTuningById("dect") && modeTuningById("dect")->stdMode == 80, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 1897.344e6 + mt.tuneOffsetHz;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 80;
    t.synth.snrDb = 25;
    t.synth.modeOpt[0] = 0;
    FileOptions fo;
    e.setStandard(80);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 79, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 8.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 79) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const DectTelemetry& d = rx.dect;
    printf("  %d reports: %s\n", reports, dectSummary(d).c_str());
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.bases.size() == 2, "%zu base stations", d.bases.size());
    CHECK(d.blocksOk >= 100, "%llu frames", (unsigned long long)d.blocksOk);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.dect().clearResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    e.stop();
    printf(fails ? "dect engine: %d FAILED\n" : "dect engine: all passed\n", fails);
    return fails ? 1 : 0;
}
