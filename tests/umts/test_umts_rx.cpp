// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The UMTS mode end to end on the test signal: the receiver thread finds both cells and the mode is registered.
#include "dect2/umts/umts_rx.h"
#include "dect2/umts/umts_cell.h"
#include "dect2/engine/mode_synth.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
// The scan runs on the receiver's own thread: wait for it (up to 60 s on a loaded runner) instead of a fixed 4 s, until both cells are in the list
static UmtsTelemetry waitCells(UmtsReceiver& rx) {
    UmtsTelemetry t;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        rx.telemetry(t, 0);
        bool a = false, b = false; for (auto& c : t.cells) { if (c.psc == 17) a = true; if (c.psc == 280) b = true; }
        if (a && b) break;
    }
    return t;
}
int main() {
    auto s = umtsGenerate({{17, 800, 1, 500, 0.4}, {280, 3000, 0.7, 9000, 0.3}}, 38400 * 2 * 8, 12);
    UmtsReceiver rx; rx.configure(7.68e6);
    if (!rx.ready()) { printf("FAIL: not ready\n"); return 1; }
    for (int r = 0; r < 5; r++) for (size_t o = 0; o < s.size(); o += 8192) rx.feed(s.data() + o, std::min<size_t>(8192, s.size() - o));
    const UmtsTelemetry t = waitCells(rx);
    printf("%zu cells, scans %llu\n", t.cells.size(), (unsigned long long)t.scans);
    bool a = false, b = false; for (auto& c : t.cells) { if (c.psc == 17) a = true; if (c.psc == 280) b = true; }
    if (!a || !b) { printf("FAIL: cells\n"); return 1; }
    ModeTuning m = umtsTuning();
    if (m.stdMode != 482 || m.minMhz >= m.defMhz) { printf("FAIL: tuning\n"); return 1; }
    // the test signal at other sample rates (3.9 sent the 7.68 Msps samples under the new rate: a time-scaled signal)
    for (double rate : {7.68e6, 10.5216e6, 15.36e6}) {
        SynthConfig sc; sc.snrDb = 12;
        auto syn = makeModeSynth(482, sc, rate);
        if (!syn) { printf("FAIL: no synth at %.2f Msps\n", rate / 1e6); return 1; }
        UmtsReceiver r2; r2.configure(syn->sampleRate());
        std::vector<cf32> buf(8192);
        for (size_t d = 0; d < (size_t)(rate * 0.4); d += buf.size()) { syn->generate(buf.data(), buf.size()); r2.feed(buf.data(), buf.size()); }
        const UmtsTelemetry t2 = waitCells(r2);
        bool a2 = false, b2 = false; for (auto& c : t2.cells) { if (c.psc == 17) a2 = true; if (c.psc == 280) b2 = true; }
        printf("%.4f Msps: %zu cells\n", rate / 1e6, t2.cells.size());
        if (!a2 || !b2) { printf("FAIL: cells at %.4f Msps\n", rate / 1e6); return 1; }
    }
    printf("umts rx passed\n");
    return 0;
}
