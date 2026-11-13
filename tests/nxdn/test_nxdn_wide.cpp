// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Whole-span decoding for NXDN, P25, dPMR, M17 and D-STAR: two generated carriers of the mode at once (different levels, offsets of +-1 kHz from
// the 12.5 / 6.25 kHz raster), one of them starting late; each must get its own decoder, lock, and put calls into the merged log with its frequency.
#include "dect2/engine/modes.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/t2/t2rx.h"
#include "dect2/engine/wide_modes.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const double kFs = 2.4e6;

static void runMode(int a, const char* name, double step, std::function<std::vector<std::pair<int, double>>(const RxTelemetry&)> logOf) {
    const ModeTuning* mt = modeTuning(a + 1);
    const double off = mt ? -mt->tuneOffsetHz : 0;   // these modes put the channel off the radio centre: the generator does the same
    const double hz[2] = {-87500 + 800 + off, 62500 - 900 + off};
    const double amp[2] = {1.0, 0.5};
    std::unique_ptr<ModeSynth> g[2];
    for (int k = 0; k < 2; k++) { SynthConfig c; c.mode = a + 1; c.snrDb = 35; c.cfoHz = hz[k] - off; g[k] = makeModeSynth(a + 1, c, kFs); if (!g[k]) { printf("FAIL: %s: no generator\n", name); fails++; return; } }
    WideModes wm; wm.configure(a, kFs, off);
    RxTelemetry t; uint64_t seq = 0;
    const size_t blk = (size_t)(kFs * 0.1);
    std::vector<cf32> sum(blk), tmp(blk);
    const double total = 30.0, lateAt = 6.0;
    size_t peak = 0;
    for (double s = 0; s < total; s += 0.1) {
        std::fill(sum.begin(), sum.end(), cf32(0, 0));
        for (int k = 0; k < 2; k++) {
            g[k]->generate(tmp.data(), blk);
            if (k == 1 && s < lateAt) continue;   // the second carrier is not on the air yet
            for (size_t i = 0; i < blk; i++) sum[i] += tmp[i] * (float)amp[k];
        }
        wm.feed(a, sum.data(), blk);
        peak = std::max(peak, wm.liveCount(a));
        wm.telemetry(a, t, seq);
    }
    printf("%s: peak %zu decoders, %zu rows\n", name, peak, t.wide.size());
    CHECK(t.wide.size() == 2, "%s: %zu carriers in the table, expected 2", name, t.wide.size());
    for (auto& r : t.wide) printf("  %+9.0f Hz  level %.1f  snr %.1f  state %d %s\n", r.hz, r.levelDb, r.snrDb, r.state, r.info.c_str());
    for (int k = 0; k < 2; k++) {
        const WideCarrier* row = nullptr; for (auto& r : t.wide) if (std::fabs(r.hz - hz[k]) < 1000) row = &r;
        CHECK(row != nullptr, "%s: carrier %d (%.0f Hz) not found", name, k, hz[k]);
        if (row) CHECK(row->state >= 1, "%s: carrier %d found but its decoder did not lock (state %d)", name, k, row->state);
        int n = 0; for (auto& e : logOf(t)) if (e.first && std::fabs(e.second - hz[k]) < 1000) n++;
        printf("  carrier %d: %d log entries\n", k, n);
        CHECK(n > 0, "%s: carrier %d: nothing in the merged log", name, k);
    }
    (void)step;
}

#define LOGOF(field) [](const RxTelemetry& t) { std::vector<std::pair<int, double>> v; for (auto& e : t.field.log) v.push_back({e.carrierId, e.carrierHz}); return v; }

int main() {
    runMode(39, "NXDN", 12500, LOGOF(nxdn));
    runMode(51, "P25", 12500, LOGOF(p25));
    runMode(46, "dPMR", 6250, LOGOF(dpmr));
    runMode(32, "M17", 12500, LOGOF(m17));
    runMode(34, "D-STAR", 12500, LOGOF(dstar));
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
