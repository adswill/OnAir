// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRAPOL through the engine across a retune: tuning to another frequency in the app resets the receiver (retuneReset). The reports must
// go on reaching latestRx() (they stopped: the reset receiver numbered them from 0 and the engine ignored them until they passed the old
// number, so the decoder looked dead until stopped and started again), and the carrier must be found again with the cell heard.
#include "dect2/engine/engine.h"
#include "dect2/tetrapol/tetrapol_gen.h"
#include "dect2/tetrapol/tetrapol_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static double wallNow(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }

// waits up to secs for reports numbered above seq; returns the seconds it took, -1 when none came
static double waitReports(Engine& e, RxTelemetry& rx, uint64_t& seq, double secs, int want) {
    const auto t0 = std::chrono::steady_clock::now();
    int got = 0;
    while (wallNow(t0) < secs) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; if (++got >= want) return wallNow(t0); }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return -1;
}

static bool cellHeard(const RxTelemetry& rx) {
    for (const auto& c : rx.tetrapol.cells)
        if (c.hasSysinfo && c.cellId == kTetrapolTestCell && c.scramble == kTetrapolTestScramble) return true;
    return false;
}

int main() {
    Engine e;
    DeviceInfo dev;                          // the synthetic source
    TuneSettings t;
    t.centerHz = 392e6;
    t.bandwidthMhz = 0.2;
    t.sampleRate = 250000;
    t.synth.mode = 81;
    t.synth.pace = 1.0;
    t.synth.cfoHz = 700;                     // a carrier offset, as a real radio has
    t.synth.snrDb = 28;
    FileOptions fo;
    e.setStandard(81);
    CHECK(e.start(dev, t, fo), "engine start");
    RxTelemetry rx;
    uint64_t seq = 0;
    CHECK(waitReports(e, rx, seq, 8.0, 20) > 0, "no reports at the start");
    const auto t0 = std::chrono::steady_clock::now();
    while (!cellHeard(rx) && wallNow(t0) < 10.0) { e.latestRx(rx, 0); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    CHECK(cellHeard(rx), "the cell was not heard before the retune");
    seq = rx.seq;
    for (int i = 0; i < 3; i++) {
        TuneSettings t2 = t;
        t2.centerHz = 392e6 + (i % 2 ? -12500.0 : 12500.0) * (i + 1);   // the next channel, as the arrow buttons do
        CHECK(e.retuneReset(t2), "retune %d", i);
        const double w = waitReports(e, rx, seq, 6.0, 10);
        printf("  retune %d (%+.1f kHz): reports again after %.2f s\n", i, (t2.centerHz - 392e6) / 1e3, w);
        CHECK(w > 0, "retune %d: the reports stopped (the decoder looked dead)", i);
        const auto t1 = std::chrono::steady_clock::now();
        bool heard = false;
        while (wallNow(t1) < 10.0 && !(heard = rx.tetrapol.synced && rx.tetrapol.blocksOk >= 30)) { e.latestRx(rx, 0); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        CHECK(heard && rx.tetrapol.synced, "retune %d: not locked again (synced %d, %llu good blocks)", i, (int)rx.tetrapol.synced, (unsigned long long)rx.tetrapol.blocksOk);
        seq = rx.seq;
    }
    e.stop();
    printf(fails ? "tetrapol engine: %d FAILED\n" : "tetrapol engine: all passed\n", fails);
    return fails ? 1 : 0;
}
