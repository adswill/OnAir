// NXDN: short voice calls (0.3 to 1 s) from a cold start, with a carrier offset, a clock error and noise (tests/common/short_burst.h). Each call must decode
// from its first frame: the voice frames counted are compared with a clean run of the same schedule. 3.2 quality pass; any argument: print only.
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/nxdn/nxdn_gen.h"
#include "dect2/nxdn/nxdn_tel.h"
#include "../common/short_burst.h"
#include "../common/test_long.h"
#include <cstring>
using namespace dect2;

int main(int argc, char** argv) {
    const bool printOnly = argc > 1;
    const double off = nxdnTuning().tuneOffsetHz;
    std::vector<NxdnGenTx> txs;
    const double durs[] = {0.32, 0.64, 0.8, 1.0};
    for (int i = 0; i < 12; i++) {
        NxdnGenTx t;
        t.src = 1001 + i; t.dst = 200 + i % 3;
        t.durSec = durs[i % (int)(sizeof durs / sizeof durs[0])];
        t.gapSec = 0.9 + 0.0137 * i;
        t.baud = (i % 2) ? 4800 : 2400;
        txs.push_back(t);
    }
    NxdnGenConfig base;
    base.rate = 1e6; base.snrDb = 40; base.seed = 3;
    base.leadSec = 0.4;
    std::vector<unsigned> ref;
    shortburst::run<NxdnGenerator, NxdnGenConfig, NxdnGenTx, NxdnReceiver, NxdnTelemetry>(base, txs, {}, off, 24, &ref);
    const shortburst::Fault faults[] = {
        {"clean 40 dB", 40, 0, 0, 0.95},
        {"20 dB", 20, 0, 0, 0.9},
        {"cfo +2.5 kHz", 25, 2500, 0, 0.9},
        {"cfo -4 kHz, +30 ppm", 25, -4000, 30, 0.9},
        {"cfo +1 kHz, 12 dB", 12, 1000, 0, 0.7},
        {"6 dB", 6, 0, 0, 0.8},
        {"cfo +3.5 kHz", 25, 3500, 0, 0.9},
        {"cfo -4.5 kHz", 25, -4500, 0, 0.9},
        {"cfo +5 kHz", 25, 5000, 0, 0.85},
        {"cfo -5 kHz", 25, -5000, 0, 0.9},
        {"cfo +6 kHz", 25, 6000, 0, 0.9},
        {"no header (late entry)", 25, 0, 0, 0.9, true},
        {"no header, 12 dB, cfo -3 kHz", 12, -3000, 0, 0.85, true},
        {"neighbour +20 dB", 25, 0, 0, 0.9, false, 20},
        {"neighbour +40 dB", 25, 0, 0, 0.8, false, 40},
    };
    int fails = 0;
    printf("%-26s %6s %8s %8s %12s %6s\n", "NXDN short calls", "calls", "decoded", "frames%", "lost/call", "extra");
    // the quick run: one seed, and of the offsets only the largest each way (the clock error, noise, late entry and neighbour cases all stay); ONAIR_TEST_LONG=1: all, 3 seeds
    const bool quick[] = {true, false, false, true, true, true, false, false, false, true, true, false, true, true, true};
    static_assert(sizeof quick / sizeof quick[0] == sizeof faults / sizeof faults[0], "one entry per fault");
    const uint32_t seeds = testLongPick(1u, 3u);
    for (size_t fi = 0; fi < sizeof faults / sizeof faults[0]; fi++) {
        const auto& f = faults[fi];
        if (argc > 2 && !strstr(f.name, argv[2])) continue;
        if (!testLong() && !quick[fi]) continue;
        double ratio = 0, lost = 0; int calls = 0, dec = 0, extra = 0;
        for (uint32_t seed = 1; seed <= seeds; seed++) {
            NxdnGenConfig c = base;
            c.snrDb = f.snr; c.cfoHz = f.cfo; c.clockPpm = f.ppm; c.seed = seed;
            auto tx2 = txs;
            if (f.noHeader) for (auto& x : tx2) x.noHeader = true;
            const auto r = shortburst::run<NxdnGenerator, NxdnGenConfig, NxdnGenTx, NxdnReceiver, NxdnTelemetry>(c, tx2, ref, off, 24, nullptr, f.neighbourDb);
            calls += r.calls; dec += r.decoded; ratio += r.ratio; lost += r.lostFrames; extra += r.extra;
        }
        const double share = (double)dec / calls;
        printf("%-26s %6d %7.0f%% %7.0f%% %12.2f %6d\n", f.name, calls, 100 * share, 100 * ratio / seeds, lost / seeds, extra);
        if (!printOnly && share < f.minShare) { printf("FAIL: %s: only %.0f%% of short calls decoded\n", f.name, 100 * share); fails++; }
    }
    printf(fails ? "%d FAILED\n" : "done\n", fails);
    return fails ? 1 : 0;
}
