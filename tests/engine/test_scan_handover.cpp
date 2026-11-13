// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A channel scan started while the receiver runs, in the scan tab's order: the receiver stops (and lets go of the radio), the scanner opens it,
// and the receiver starts again once the scan is over. The very first scan must measure and lock the channel that carries a DVB-T2 signal,
// as the second one does, and list only that channel: not the empty one, nor the one with a signal of another kind (ATSC).
#include "dect2/engine/scanner.h"
#include "../common/ci_slack.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace dect2;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static void waitSec(double s) { std::this_thread::sleep_for(std::chrono::milliseconds((int)(s * 1000))); }

int main() {
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "test signal";
    FileOptions fo;
    TuneSettings rxTune; rxTune.centerHz = 482e6; rxTune.bandwidthMhz = 8; rxTune.sampleRate = 10e6;
    Engine rx;
    CHECK(rx.start(dev, rxTune, fo), "the receiver starts");
    waitSec(1.0);
    for (int run = 1; run <= 2; run++) {
        ScanConfig cfg;
        cfg.startMHz = 474; cfg.stopMHz = 490; cfg.stepMHz = 8; cfg.bwMhz = 8;
        cfg.identifyServices = false;
        cfg.lockTimeoutSec = 15 * ciSlack();   // a busy machine running the tests in parallel needs longer than a radio does
        cfg.testTune = [](double f, TuneSettings& t) {
            t.synth.snrDb = 30;
            t.sampleRate = 64e6 / 7;   // the DVB-T2 test signal's own rate (8 MHz): the silence and the ATSC signal run at it too, as one radio would
            t.synth.atsc = std::abs(f - 490) < 0.01;                                  // 490: ATSC, a signal but not DVB
            t.synth.mode = std::abs(f - 482) < 0.01 || t.synth.atsc ? 0 : 99;        // 482: DVB-T2; 474: silence
        };
        const bool wasRunning = rx.running();
        CHECK(wasRunning, "the receiver runs when the scan starts");
        if (wasRunning) rx.stop();   // as the scan tab does: the radio is closed when stop() returns
        CHECK(!rx.running(), "the receiver let go of the radio");
        Scanner s;
        std::string err;
        CHECK(s.start(dev, cfg, err), "the scan starts");
        for (int i = 0; i < 1200 && (s.progress().running || i < 5); i++) waitSec(0.1);
        const ScanProgress pr = s.progress();
        const auto res = s.results();
        printf("  scan %d: %s\n", run, pr.phase.c_str());
        CHECK(!pr.running && pr.phase == "finished", "the scan finished");
        CHECK(res.size() == 3, "three channels were scanned");
        int seen = 0;
        for (const auto& r : res) {
            printf("    %.1f MHz  level %.1f dBFS  occupied %d locked %d seen %d  %s\n", r.freqMHz, r.levelDbfs, (int)r.occupied, (int)r.t2, (int)r.seen, r.note.c_str());
            if (r.seen) seen++;
            if (std::abs(r.freqMHz - 482) < 0.01) CHECK(r.t2 && r.seen && r.standard == "DVB-T2", "the DVB-T2 channel is locked on this scan");
            else CHECK(!r.seen && !r.t2, "a channel without DVB is not listed");
            if (std::abs(r.freqMHz - 490) < 0.01) CHECK(r.occupied, "the ATSC channel was measured (the scan saw samples)");
        }
        CHECK(seen == 1, "only the DVB-T2 channel is listed");
        s.stop();
        if (wasRunning) CHECK(rx.start(dev, rxTune, fo), "the receiver starts again after the scan");
        waitSec(0.5);
    }
    rx.stop();
    printf(fails ? "scan handover: FAILED\n" : "scan handover: ok\n");
    return fails ? 1 : 0;
}
