// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PSK31 through the engine (setStandard(38)): the synthetic source plays the test band, the receiver reports through latestRx().
#include "dect2/psk/psk_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = pskTuning();
    CHECK(mt.stdMode == 38 && modeTuningById("psk") && modeTuningById("psk")->stdMode == 38, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 38;
    t.synth.snrDb = 14;
    FileOptions fo;
    e.setStandard(38);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 37, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 16.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 37) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const PskTelemetry& p = rx.psk;
    int found = 0;
    for (const auto& s : p.signals) {
        printf("  %7.1f Hz %4.1f dB \"%s\"\n", s.offsetHz, s.snrDb, s.text.c_str());
        if (s.text.find("LY2ABC") != std::string::npos || s.text.find("G4ABC") != std::string::npos || s.text.find("OK1XX") != std::string::npos || s.text.find("EA3XYZ") != std::string::npos) found++;
    }
    printf("  %d reports: %s\n", reports, pskSummary(p).c_str());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(found >= 3, "%d signals decoded", found);
    CHECK(!p.spectrum.empty(), "spectrum");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "psk engine: %d FAILED\n" : "psk engine: all passed\n", fails);
    return fails ? 1 : 0;
}
