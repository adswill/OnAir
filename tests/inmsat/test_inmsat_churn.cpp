// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Issue #508: in the all-carriers mode a window that opens (a new carrier far away) used to configure() every other window's receivers
// again, which restarts them: Aero locks and half-received multi-block messages gone, and the Inmarsat-C receiver (8.64 s frames, a long
// carrier search) never getting to lock on a busy band. Here an Aero group and an Inmarsat-C channel run alone, a second Aero group 2 MHz
// away appears after 45 s, and the first group must stay in sync across that moment. The same capture must give positions (ADS-C and the
// text POS report of the generators) in the merged result. Synthetic signals only; needs a user test with a real capture.
#include "dect2/inmsat/inmsat_manager.h"
#include "dect2/aero/aero_gen.h"
#include "dect2/aero/aero_rx.h"
#include "dect2/inmc/inmc_testutil.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::inmsat;
#include "../common/test_util.h"

namespace {
struct Shifter {
    double f, fs, ph = 0;
    void apply(cf32* x, size_t n) {
        const double w = 2 * M_PI * f / fs;
        for (size_t i = 0; i < n; i++) { x[i] *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += w; if (ph > 2 * M_PI) ph -= 2 * M_PI; }
    }
};
}

int main() {
    const double rate = 6e6;
    SynthConfig a1, a2;
    a1.mode = a2.mode = 20;
    a1.modeOpt[0] = 7;  a1.modeVal[0] = 17;
    a2.modeOpt[0] = 4;  a2.modeOpt[1] = 3; a2.modeVal[0] = 17;
    AeroSynth s1(a1, rate), s2(a2, rate);
    InmcGenConfig g;
    g.rate = rate; g.ebn0Db = 17; g.seed = 5; g.extraChannels = 1;
    auto inm = makeInmcGenerator(g);
    Shifter sh2{-2.0e6, rate}, shI{+1.0e6, rate};

    ManagerConfig mc;
    mc.rate = rate;
    mc.centerHz = 1545.0e6;
    Manager mgr;
    mgr.configure(mc);
    const double churnAt = 45, endAt = 75;
    const size_t chunk = 65536;
    std::vector<cf32> b1(chunk), b2(chunk), bi(chunk), mix(chunk);
    Snapshot snap;
    uint64_t last = 0;
    int inSyncBefore = 0, minAfter = 1 << 30, windowsBefore = 0, windowsAfter = 0;
    uint64_t posBefore = 0;
    for (size_t done = 0; done < (size_t)(endAt * rate); done += chunk) {
        const double t = (double)done / rate;
        s1.generate(b1.data(), chunk);
        s2.generate(b2.data(), chunk); sh2.apply(b2.data(), chunk);
        inm->generate(bi.data(), chunk); shI.apply(bi.data(), chunk);
        for (size_t i = 0; i < chunk; i++) mix[i] = b1[i] + bi[i] + (t >= churnAt ? b2[i] : cf32());
        mgr.feed(mix.data(), chunk);
        if (!mgr.snapshot(snap, last)) continue;
        last = snap.seq;
        int nearIn = 0;      // carriers of the first group (inside +-300 kHz of the centre) in sync
        for (const auto& c : snap.carriers) nearIn += std::fabs(c.offsetHz) < 300e3 && c.type == CarrierType::AeroP && c.state >= 2;
        if (t < churnAt - 1) { inSyncBefore = nearIn; windowsBefore = (int)snap.windows.size(); posBefore = snap.positionsTotal; }
        else if (t > churnAt + 0.6 && t < churnAt + 8) { minAfter = std::min(minAfter, nearIn); windowsAfter = std::max(windowsAfter, (int)snap.windows.size()); }
    }
    printf("churn: Aero carriers in sync before %d, lowest after the new window %d, windows %d -> %d, positions before %llu, total %llu\n", inSyncBefore, minAfter, windowsBefore, windowsAfter,
           (unsigned long long)posBefore, (unsigned long long)snap.positionsTotal);
    CHECK(inSyncBefore >= 2, "the first group was not in sync before the new window: %d", inSyncBefore);
    CHECK(windowsAfter > windowsBefore, "the second group did not open a window: %d -> %d", windowsBefore, windowsAfter);
    CHECK(minAfter >= inSyncBefore - 1, "a new window restarted the others: %d in sync before, %d after", inSyncBefore, minAfter);
    CHECK(snap.positionsTotal > 0, "no position decoded from the P channels of a multi-carrier capture");
    int placed = 0;
    for (const auto& a : snap.aircraft) placed += a.hasPos;
    CHECK(placed >= 2, "aircraft with a position in the merged list: %d of %zu", placed, snap.aircraft.size());
    int inmc = 0;
    for (const auto& c : snap.carriers) inmc += c.type == CarrierType::InmarsatC && c.state >= 2;
    CHECK(inmc >= 1, "the Inmarsat-C carrier is not decoded after the new window: %d", inmc);
    return testSummary("inmsat_churn");
}
