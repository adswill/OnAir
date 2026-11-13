// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL through the engine on the synthetic source (setStandard(124)): the built-in test signal is the real transmitter's output (voice, sweep ID, beacon) with a
// carrier offset of +1200 Hz and a little noise; the receiver must lock, decode frames and read the beacon. Run at 12 / 16 and at 24 kbit/s (16 ksym/s).
#include "dect2/engine/engine.h"
#include "dect2/kaal/kaal_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static void run(int kb, double secs) {
    const ModeTuning mt = kaalTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 434.92e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 124;
    t.synth.modeOpt[0] = kb;
    FileOptions fo;
    e.setStandard(124);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 123, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    double lockAt = -1;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 90.0) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq; reports++;
            if (rx.standard != 123) wrong++;
            if (rx.kaal.state == 2 && lockAt < 0) lockAt = rx.kaal.timeSec;
            if (rx.kaal.timeSec > secs) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const KaalTelemetry& k = rx.kaal;
    printf("  %d kbit/s: %d reports: %s, lock at %.1f s, frames ok %llu bad %llu, cfo %.0f Hz, %d ksym/s, beacon %s \"%s\"\n", kb, reports, kaalSummary(k).c_str(), lockAt,
           (unsigned long long)k.blocksOk, (unsigned long long)k.blocksBad, k.cfoHz, k.symRate / 1000, k.callsign.c_str(), k.text.c_str());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(lockAt > 0 && lockAt < 14, "lock at %.1f s", lockAt);
    CHECK(k.blocksOk > 200 && k.blocksBad * 20 < k.blocksOk, "frames ok %llu bad %llu", (unsigned long long)k.blocksOk, (unsigned long long)k.blocksBad);
    CHECK(std::fabs(k.cfoHz - 1200.0) < 150.0, "carrier offset read %.0f Hz, sent 1200", k.cfoHz);
    CHECK(k.symRate == (kb >= 24 ? 16000 : 8000), "symbol rate %d", k.symRate);
    CHECK(k.beaconValid && k.callsign == "ONAIR", "beacon '%s'", k.callsign.c_str());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
}

int main() {
    const ModeTuning mt = kaalTuning();
    CHECK(mt.stdMode == 124 && modeTuningById("kaal") && modeTuningById("kaal")->stdMode == 124, "tuning table");
    run(16, 20);
    run(24, 14);
    return testSummary("kaal synth");
}
