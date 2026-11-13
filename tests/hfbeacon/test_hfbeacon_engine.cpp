// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF beacons through the engine (setStandard(69)): the synthetic source plays single-letter beacons D (3594.7 kHz) and K (3595.3 kHz) keyed at 15 dB
// in 100 Hz, and a steady carrier at 3595.45 kHz, 1 Msps around 3.65 MHz; the engine reports them as standard 68 with D and K heard (identity from
// the keying), the carrier and the silent markers not. Also: the beacon list pushed from outside, results save / load / clear, the tuning table.
#include "dect2/engine/engine.h"
#include "dect2/hfbeacon/hfbeacon_gen.h"
#include "dect2/hfbeacon/hfbeacon_rx.h"
#include "dect2/hfbeacon/hfbeacon_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"
int main() {
    const ModeTuning mt = hfbeaconTuning();
    CHECK(mt.stdMode == 69 && modeTuningById("hfbeacon") && modeTuningById("hfbeacon")->stdMode == 69, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = kHfbSynthDefMhz * 1e6;
    t.bandwidthMhz = 1;
    t.sampleRate = 1e6;
    t.synth.mode = 69;
    FileOptions fo;
    e.setStandard(69);
    e.hfbeacon().setBeacons(hfb::defaultBeacons());
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 68, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 16) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 68) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const HfbTelemetry& s = rx.hfb;
    printf("  %d reports: %s\n", reports, hfbeaconSummary(s).c_str());
    CHECK(reports > 8 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk && s.state == 1 && s.beacons.size() == hfb::defaultBeacons().size(), "state %d, %zu beacons", s.state, s.beacons.size());
    const auto list = hfb::defaultBeacons();
    int heardD = 0, heardK = 0, other = 0, inSpan = 0;
    for (size_t i = 0; i < list.size(); i++) {
        inSpan += s.beacons[i].inSpan;
        if (!s.beacons[i].heard) continue;
        if (list[i].kind == hfb::Letter && std::fabs(list[i].khz - 3594.7) < 0.01) heardD++;
        else if (list[i].kind == hfb::Letter && std::fabs(list[i].khz - 3595.3) < 0.01) heardK++;
        else other++;
    }
    for (size_t i = 0; i < list.size(); i++) if (s.beacons[i].inSpan) printf("  %-40s heard %d conf %d snr %.1f %s\n", list[i].name.c_str(), s.beacons[i].heard, s.beacons[i].conf, (double)s.beacons[i].snr, s.beacons[i].note.c_str());
    CHECK(inSpan == 8, "%d beacons in the span (the 3.59 MHz cluster)", inSpan);
    CHECK(heardD == 1 && heardK == 1 && other == 0, "heard D %d, K %d, others %d", heardD, heardK, other);
    CHECK(s.bands[5].heard == 2 && s.bands[5].inSpan >= 8, "band summary: %d heard, %d in span", s.bands[5].heard, s.bands[5].inSpan);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    // results: written by the engine's slot, cleared, loaded
    const auto names = e.resultNames();
    bool has = false;
    for (const auto& n : names) has = has || n == "hfbeacon";
    CHECK(has, "no results slot");
    const std::string blob = e.hfbeacon().saveResults();
    CHECK(!blob.empty(), "blob");
    e.stop();
    HfbReceiver r;
    CHECK(r.loadResults(blob), "load");
    CHECK(!r.loadResults("x"), "garbage accepted");
    printf(fails ? "hfbeacon engine: %d FAILED\n" : "hfbeacon engine: all passed\n", fails);
    return fails ? 1 : 0;
}
