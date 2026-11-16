// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Issue #542: the waterfall froze for about half a second every few seconds while ISM sensors were decoded. The engine feeds the receiver
// from the same thread that feeds the spectrum, so a feed() call that decodes a burst inline holds the waterfall. Here: the time of the
// longest feed() call of 8 ms of input over a busy scene, with the faults that make bursts long (a hot noisy band), and over a retune.
#include "dect2/ism/ism_gen.h"
#include "dect2/ism/ism_rx.h"
#include "dect2/ism/ism_tel.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRate = 2e6, kDial = -250e3;
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

struct Stat { double maxMs = 0, totalMs = 0; size_t calls = 0; std::vector<double> t; };

static double pct(std::vector<double> v, double q) { std::sort(v.begin(), v.end()); return v.empty() ? 0 : v[std::min(v.size() - 1, (size_t)(q * (double)v.size()))]; }
// the shared verdict: a runner can stall a call now and then (a page fault, a preempted thread), and the first call of a receiver sets it up (tables,
// the ring, the worker thread), so the check is that the thread is almost always free (95th percentile under 60 ms) and no call is anywhere near
// the half second the bug cost every few seconds.
static void judge(const char* what, const Stat& s) {
    size_t over = 0;
    for (double d : s.t) over += d > 20;
    printf("  %-18s %zu calls: median %.2f ms, 95th %.2f ms, max %.1f ms, %zu over 20 ms\n", what, s.calls, pct(s.t, 0.5), pct(s.t, 0.95), s.maxMs, over);
    CHECK(pct(s.t, 0.95) < 60, "%s: 95th percentile of feed() %.1f ms", what, pct(s.t, 0.95));
    CHECK(s.maxMs < 500, "%s: one feed() call held the thread %.0f ms", what, s.maxMs);
    CHECK(over * 50 < s.calls, "%s: %zu of %zu calls over 20 ms", what, over, s.calls);
}

static Stat timeFeeds(IsmReceiver& rx, const std::vector<cf32>& x, bool paced = false, size_t chunk = 16384) {
    Stat s;
    for (size_t i = 0; i < x.size(); i += chunk) {
        const auto t0 = Clock::now();
        rx.feed(x.data() + i, std::min(chunk, x.size() - i));
        const double d = ms(t0, Clock::now());
        if (d > s.maxMs && getenv("ISM_STALL_DBG")) printf("    new max %.1f ms at call %zu\n", d, s.calls);
        s.maxMs = std::max(s.maxMs, d); s.totalMs += d; s.calls++; s.t.push_back(d);
        if (d > 20) printf("    slow call %zu: %.1f ms\n", s.calls - 1, d);
        if (paced) std::this_thread::sleep_for(std::chrono::milliseconds(4));   // the radio delivers 8 ms of input per call, not all at once
    }
    return s;
}

int main() {
    IsmGenConfig c;
    c.rate = kRate; c.dialOffsetHz = kDial; c.snrDb = 30; c.seed = 11;
    c.devs = ismTestScene(0);
    IsmGenerator g(c);
    std::vector<cf32> x((size_t)(12 * kRate));
    g.generate(x.data(), x.size());
    // a hot band: strong wideband noise bursts of 0.2 .. 0.6 s (a neighbour's transmitter, a motor, a switching supply) on top of it
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0.f, 1.f);
    std::vector<cf32> y = x;
    for (double t0 = 0.7; t0 < 11.0; t0 += 2.9) {
        const size_t a = (size_t)(t0 * kRate), n = (size_t)((0.3 + 0.1 * std::fmod(t0, 3)) * kRate);
        for (size_t i = a; i < a + n && i < y.size(); i++) y[i] += cf32(0.3f * nd(rng), 0.3f * nd(rng));
    }
    for (int k = 0; k < 2; k++) {
        IsmReceiver rx;
        rx.configure(kRate);
        rx.setSignalOffset(kDial);
        rx.setRealtime(true);   // what the engine sets for a radio
        const Stat s = timeFeeds(rx, k ? y : x);
        judge(k ? "noise bursts" : "clean scene", s);
    }
    {   // a retune: reset and a new centre, then the input goes on; the next call must not wait on anything
        IsmReceiver rx;
        rx.configure(kRate);
        rx.setSignalOffset(kDial);
        rx.setRealtime(true);   // what the engine sets for a radio
        timeFeeds(rx, std::vector<cf32>(y.begin(), y.begin() + (size_t)(3 * kRate)));
        rx.setCenterHz(868.3e6);
        rx.reset();
        const Stat s = timeFeeds(rx, std::vector<cf32>(y.begin() + (size_t)(3 * kRate), y.begin() + (size_t)(8 * kRate)), true);
        rx.flush();
        IsmTelemetry t;
        rx.telemetry(t, 0);
        judge("after a retune", s);
        printf("    %.1f s processed, %zu devices\n", t.timeSec, t.devices.size());
        CHECK(t.timeSec > 4, "after a retune the receiver took the input again: only %.1f s of 5", t.timeSec);
    }
    return testSummary("ism_stall");
}
