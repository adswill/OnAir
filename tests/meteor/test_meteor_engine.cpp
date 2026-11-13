// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor-M LRPT through the engine (setStandard(61)): the synthetic source plays the test signal (OQPSK 72k, 1 Msps, offset tuning),
// the receiver reports through latestRx() and the picture grows. No sound.
#include "dect2/meteor/meteor_rx.h"
#include "dect2/engine/modes.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = meteorTuning();
    CHECK(mt.stdMode == 61 && modeTuningById("meteor") && modeTuningById("meteor")->stdMode == 61, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 61;
    t.synth.snrDb = 18;
    FileOptions fo;
    e.setStandard(61);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 60, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 8.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 60) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const MeteorTelemetry& d = rx.meteor;
    printf("  %d reports: %s, %llu frames, %llu bad, %d rows\n", reports, meteorSummary(d).c_str(), (unsigned long long)d.blocksOk, (unsigned long long)d.blocksBad,
           d.image ? d.image->rows : 0);
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.blocksOk > 10 && d.image && d.image->rows >= 3 && d.image->packets[0] > 0, "frames %llu", (unsigned long long)d.blocksOk);
    CHECK(modeSummary(rx).find("OQPSK") != std::string::npos, "summary %s", modeSummary(rx).c_str());
    e.stop();
    printf(fails ? "FAILED (%d)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
