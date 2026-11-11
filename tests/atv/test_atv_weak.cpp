// Analog TV receiver on weak signals: PAL, NTSC and SECAM test cards at carrier-to-noise ratios down to 5 dB. The line sync is found by folding the
// signal at the line period (a matched filter over hundreds of lines) and followed by matched filters at the slots the loop expects, the picture
// is cleaned by the temporal filter. The picture must lock, have the right raster and stay close to the card (mean luminance difference, 0..255);
// noise alone must not lock. Before this the lock was lost below about 12 dB, and the picture was noise from 16 dB down (luma difference 40 and more).
#include "dect2/atv/atv_testkit.h"
#include "dect2/common/stable_random.h"
#include "../common/jobs.h"
#include "../common/test_long.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atvkit;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

int main() {
    struct Sys { int sys, colour, lines; const char* name; };
    const Sys systems[] = {{kAtvG, kAtvPal, 625, "PAL B/G"}, {kAtvM, kAtvNtsc, 525, "NTSC M"}, {kAtvDK, kAtvSecam, 625, "SECAM D/K"}};
    // carrier-to-noise ratio (dB) and the largest luminance difference to the card that still counts as a picture
    struct Level { double cnr, maxLuma; };
    const Level levels[] = {{16, 30}, {12, 42}, {8, 54}, {5, 64}};
    testjobs::Jobs jobs;
    // the default run: every system at the weakest level and PAL at the strongest too, and 3 s of noise; ONAIR_TEST_LONG=1 every level and 10 s of noise
    for (const Sys& s : systems) for (const Level& l : levels) {
        if (!testLong() && !(l.cnr == 5 || (s.sys == kAtvG && l.cnr == 16))) continue;
        jobs.add([=] {
        AtvFormat f; atvMakeFormat(s.sys, s.colour, f);
        AtvGenConfig c; c.sys = s.sys; c.colour = s.colour; c.cnrDb = l.cnr; c.rate = 10e6; c.sound = 4;
        c.cfoHz = 120e3;                                         // and a carrier that is not where the channel plan has it
        const Run r = run(c, 7.0);
        const double luma = r.frame ? lumaDifference(*r.frame, AtvCard(f), 3) : 999;
        jprintf("%-10s C/N %2.0f dB: state %d, %d lines, locked after %d ms, luma difference %.1f\n", s.name, l.cnr, r.tel.state, r.tel.lines, r.lockedAfterMs, luma);
        CHECK(r.tel.state == 2, "%s C/N %.0f: state %d", s.name, l.cnr, r.tel.state);
        CHECK(r.tel.lines == s.lines, "%s C/N %.0f: %d lines", s.name, l.cnr, r.tel.lines);
        CHECK(luma < l.maxLuma, "%s C/N %.0f: luminance difference %.1f (limit %.0f)", s.name, l.cnr, luma, l.maxLuma);
    }); }
    // noise alone, for a long time: no lock, no picture
    jobs.add([] {
        AtvReceiver rx;
        rx.setSilent(true);
        rx.configure(10e6);
        std::mt19937 rng(11);
        dect2::normal_distribution<float> g(0.f, 0.08f);
        std::vector<cf32> x(65536);
        uint64_t fs = 0;
        int frames = 0, locked = 0;
        const int chunks = testLongPick(48, 160);
        for (int k = 0; k < chunks; k++) {                       // about 1 s each 15 chunks: 3 s (10 s in the long run)
            for (auto& v : x) v = cf32{g(rng), g(rng)};
            rx.feed(x.data(), x.size());
            if (rx.frame(fs)) frames++;
            AtvTelemetry t;
            if (rx.telemetry(t, 0) && t.state == 2) locked++;
        }
        jprintf("noise only: %d pictures, %d reports locked\n", frames, locked);
        CHECK(frames == 0 && locked == 0, "noise gave %d pictures and %d locked reports", frames, locked);
    });
    jobs.run();
    if (fails) { jprintf("%d check(s) failed\n", fails.load()); return 1; }
    jprintf("OK\n");
    return 0;
}
