// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Drone Remote ID through the engine (setStandard(58)): the synthetic source plays the test schedule, the receiver reports through
// latestRx(). No sound.
#include "dect2/rid/rid_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = ridTuning();
    CHECK(mt.stdMode == 58 && modeTuningById("rid") && modeTuningById("rid")->stdMode == 58, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 58;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(58);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 57, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 20.0 && !(reports > 4 && rx.rid.drones.size() == 2 && rx.rid.bleRid > 50 && rx.rid.wifiRid > 50)) {   // until both drones are well heard (a loaded machine runs the test signal slower)
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 57) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const RidTelemetry& d = rx.rid;
    printf("  %d reports: %s, %llu frames, %llu bad\n", reports, ridSummary(d).c_str(), (unsigned long long)d.blocksOk, (unsigned long long)d.blocksBad);
    for (const auto& x : d.drones) printf("  %s %.6f %.6f op %.6f %.6f\n", x.mac.c_str(), x.lat, x.lon, x.opLat, x.opLon);
    CHECK(reports > 4 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.drones.size() == 2 && d.bleRid > 50 && d.wifiRid > 50, "%zu drones, %llu BLE, %llu Wi-Fi", d.drones.size(), (unsigned long long)d.bleRid, (unsigned long long)d.wifiRid);
    CHECK(d.bleChannel == 38 && d.wifiChannel == 6, "channels %d %d", d.bleChannel, d.wifiChannel);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "rid engine: %d FAILED\n" : "rid engine: all passed\n", fails);
    return fails ? 1 : 0;
}
