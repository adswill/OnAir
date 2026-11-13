// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Why the input ring overflowed (core/include/dect2/engine/overflow_cause.h), on a simulated sample path: a radio writing transfers into a
// ring of the engine's size, and a sample thread that runs the engine's loop (catch-up when more than half of the ring is backlog, read
// pieces of 65536, a pass every 33 ms, a mark per pass) with a receiver of a given cost and share on the CPU, and hold-ups outside it.
// 3.9 called every overflow "this computer is too slow"; a fast idle machine that was held up once must not be told that, and a receiver
// that really needs more than real time on the CPU must be.
#include "dect2/engine/overflow_cause.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Path {
    double rate = 10e6;
    double cost = 0.3;       // receive-path wall seconds per signal second
    double onCpu = 1.0;      // share of that time on the CPU (the rest: waiting for workers, a lock, or the OS)
    double holdAt = -1, holdSec = 0;   // the sample thread is held up outside the receiver once
    double seconds = 30;
    size_t block = 65536;
    size_t capacity = 1u << 24;   // the engine's ring
};

struct Seen { int overflows = 0; int byCause[5] = {}; };

static Seen simulate(const Path& p) {
    LoopMarks marks;
    const int64_t t0 = 5000000000000LL;
    auto ns = [&](double t) { return t0 + (int64_t)std::llround(t * 1e9); };
    double t = 0, next = 0;
    uint64_t w = 0, r = 0, dropped = 0, seenDropped = 0, blocks = 0;
    double rx = 0, rxCpu = 0, samp = 0;
    bool held = false;
    Seen s;
    auto produce = [&](double now) {   // the radio's transfers up to now; what does not fit is dropped
        while ((double)(blocks + 1) * (double)p.block / p.rate <= now) {
            blocks++;
            const uint64_t freeN = p.capacity - (w - r);
            const uint64_t m = std::min<uint64_t>(p.block, freeN);
            w += m; dropped += p.block - m;
        }
    };
    auto consume = [&](uint64_t n) {
        const double d = (double)n / p.rate * p.cost;
        t += d; rx += d; rxCpu += d * p.onCpu; samp += (double)n; r += n;
        produce(t);
    };
    while (t < p.seconds) {
        produce(t);
        if (w - r > p.capacity / 2) { const uint64_t keep = p.capacity / 16; dropped += (w - r) - keep; r = w - keep; }   // catchUp()
        while (w > r) { consume(std::min<uint64_t>(w - r, 65536)); if (t > next + 0.25) break; }
        next += 0.033;
        if (!held && p.holdAt >= 0 && t >= p.holdAt) { held = true; t += p.holdSec; produce(t); }   // outside the receiver
        marks.push(ns(t), rx, rxCpu, samp);
        if (dropped != seenDropped) {
            seenDropped = dropped;
            const OverflowCause c = classifyOverflow(marks.window(p.rate, ns(0), ns(t)));
            s.overflows++; s.byCause[(int)c]++;
        }
        while (t < next) {   // the wait between passes: read what comes, else sleep to the next pass
            produce(t);
            if (w > r) consume(std::min<uint64_t>(w - r, 65536));
            else t = std::min(next, (double)(blocks + 1) * (double)p.block / p.rate);
        }
    }
    return s;
}

static void show(const char* name, const Seen& s) {
    printf("  %-58s %d overflows: starting %d, cpu %d, waiting %d, stalled %d\n", name, s.overflows, s.byCause[1], s.byCause[2], s.byCause[3], s.byCause[4]);
}

int main() {
    // the pure rules
    {
        OverflowWindow w; w.wallSec = 2; w.signalSec = 1.5; w.busyWallSec = 1.95; w.busyCpuSec = 1.9;
        CHECK(classifyOverflow(w) == OverflowCause::Cpu, "behind on the CPU");
        w.busyCpuSec = 0.4;
        CHECK(classifyOverflow(w) == OverflowCause::Waiting, "behind, waiting");
        w.busyWallSec = 0.3; w.busyCpuSec = 0.3; w.signalSec = 2;
        CHECK(classifyOverflow(w) == OverflowCause::Stalled, "keeps up: held up");
        w.sinceStartSec = 1;
        CHECK(classifyOverflow(w) == OverflowCause::Starting, "at the start");
    }
    // a receiver that needs 130 % of real time on the CPU, at 10 and 20 Msps: every overflow after the start is Cpu
    for (double rate : {10e6, 20e6}) {
        Path p; p.rate = rate; p.cost = 1.3; p.onCpu = 1.0;
        const Seen s = simulate(p);
        show(rate > 15e6 ? "20 Msps, receiver needs 130 % of real time on the CPU" : "10 Msps, receiver needs 130 % of real time on the CPU", s);
        CHECK(s.overflows >= 3 && s.byCause[2] == s.overflows - s.byCause[1], "short of CPU: %d overflows, %d cpu", s.overflows, s.byCause[2]);
    }
    // the same need, but only 20 % of it on the CPU (it waits for its workers or for the OS): Waiting, never Cpu
    {
        Path p; p.cost = 1.3; p.onCpu = 0.2;
        const Seen s = simulate(p);
        show("10 Msps, 130 % of real time, 20 % of it on the CPU", s);
        CHECK(s.overflows >= 3 && s.byCause[2] == 0 && s.byCause[3] == s.overflows - s.byCause[1], "waiting: %d overflows, %d cpu, %d waiting", s.overflows, s.byCause[2], s.byCause[3]);
    }
    // a fast machine (the receiver at 10 % of real time) held up once for 2 s: Stalled, never "too slow" (the Discord report: 3 % CPU)
    for (double rate : {2.4e6, 10e6, 20e6}) {
        Path p; p.rate = rate; p.cost = 0.1; p.holdAt = 12; p.holdSec = rate > 15e6 ? 1.0 : rate > 5e6 ? 2.0 : 9.0;
        const Seen s = simulate(p);
        char name[96]; snprintf(name, sizeof name, "%.1f Msps, receiver at 10 %%, held up %.0f s once", rate / 1e6, p.holdSec);
        show(name, s);
        CHECK(s.overflows >= 1 && s.byCause[2] == 0 && s.byCause[3] == 0 && s.byCause[4] == s.overflows, "held up: %d overflows, %d cpu", s.overflows, s.byCause[2]);
    }
    // the same hold-up two seconds after the start: the start's own
    {
        Path p; p.cost = 0.1; p.holdAt = 2; p.holdSec = 2;
        const Seen s = simulate(p);
        show("10 Msps, held up 2 s right after the start", s);
        CHECK(s.overflows >= 1 && s.byCause[1] == s.overflows, "starting: %d overflows, %d starting", s.overflows, s.byCause[1]);
    }
    // a receiver that keeps up (90 % of real time) never overflows, at a low and a high rate
    for (double rate : {250e3, 10e6}) {
        Path p; p.rate = rate; p.cost = 0.9; p.block = rate < 1e6 ? 131072 : 65536;
        const Seen s = simulate(p);
        CHECK(s.overflows == 0, "keeps up at %.2f Msps: %d overflows", rate / 1e6, s.overflows);
    }
    printf(fails ? "overflow cause: FAILED\n" : "overflow cause: ok\n");
    return fails ? 1 : 0;
}
