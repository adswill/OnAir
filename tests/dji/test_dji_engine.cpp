// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DJI drones through the engine (setStandard(70)): the synthetic source plays DroneID bursts and an OcuSync-like train at 20 Msps, the receiver reports
// through latestRx(). No sound. Also the recording of that signal with SigMF metadata is not tested here (tests/source/test_sigmf.cpp does that).
#include "dect2/dji/dji_rx.h"
#include "dect2/dji/dji_gen.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = djiTuning();
    CHECK(mt.stdMode == 70 && modeTuningById("dji") && modeTuningById("dji")->stdMode == 70 && modeTuning(70) && std::string(modeTuning(70)->name) == "DJI drones", "tuning table");
    CHECK(minSampleRateFor(70, 8) >= 16e6, "minimum rate %.1f", minSampleRateFor(70, 8));
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 70;
    t.synth.snrDb = 25;
    t.synth.modeOpt[0] = 0;       // DroneID + OcuSync-like + sparse Wi-Fi-like bursts
    t.synth.modeVal[0] = 0.4;     // a DroneID burst every 0.4 s
    FileOptions fo;
    e.setStandard(70);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 69, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 9.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 69) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const DjiTelemetry& d = rx.dji;
    printf("  %d reports: %s, %zu drones, %zu tracks, %llu packets; dropped %llu samples\n", reports, djiSummary(d).c_str(), d.drones.size(), d.tracks.size(),
           (unsigned long long)d.blocksOk, (unsigned long long)e.droppedSamples());
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.drones.size() == 1 && d.drones[0].serial == kDjiGenSerial && d.blocksOk >= 2, "DroneID: %zu drones, %llu packets", d.drones.size(), (unsigned long long)d.blocksOk);
    bool train = false;
    for (const auto& k : d.tracks) train = train || k.cls == kDjiOcusync;
    CHECK(train, "the OcuSync-like train was not recognised");
    e.dji().clearResults();
    e.stop();
    printf(fails ? "dji engine: %d FAILED\n" : "dji engine: all passed\n", fails);
    return fails ? 1 : 0;
}
