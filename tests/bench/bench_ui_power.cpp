// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The interface's idle and active power, without a window: the numbers docs/PERFORMANCE.md quotes.
//   frames   redraws per second the frame pacing asks for in each window state (app/ui/frame_pace.h)
//   ring     the analysis thread's wake-ups and CPU while it waits for samples: the old 2 ms sleep loop against IqRing::waitData()
//   tr       the cost of one tr() lookup (English, and a translation with "Label##id" keys)
//   rfcam    the RF camera's per-frame picture blend: every frame against only when the result changed
// Usage: bench_ui_power [section...] (all when none). Prints a table per section; no pass/fail.
#include "../../app/ui/frame_pace.h"
#include "../../app/i18n/i18n.h"
#include "dect2/common/ring.h"
#include "dect2/df/rfcam.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;
using dect2::cf32;

// every heap allocation is counted (tr() should make none once its caches are warm)
static std::atomic<long> gAllocs{0};
void* operator new(size_t n) { gAllocs++; if (void* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static double threadCpu() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// the policy paceFrame() had before frame_pace.h (0.2.9): no occlusion, no battery
static double legacyInterval(const pace::State& s) {
    if (s.shot) return 0;
    if (!s.shown && !s.popOut && !s.noOsWindows) return s.reduceCpuHidden ? 0.1 : 0.25;
    if (s.recentInput || s.video) return 1.0 / 60;
    if (s.running) return s.focused ? 1.0 / 30 : 1.0 / 15;
    return s.focused ? 0.1 : 0.25;
}

static void frames() {
    struct Row { const char* name; pace::State s; };
    auto st = [](auto f) { pace::State s; f(s); return s; };
    const Row rows[] = {
        {"idle, focused", st([](pace::State&) {})},
        {"idle, in the background", st([](pace::State& s) { s.focused = false; })},
        {"receiving, focused", st([](pace::State& s) { s.running = true; })},
        {"receiving, background", st([](pace::State& s) { s.running = true; s.focused = false; })},
        {"receiving, covered window", st([](pace::State& s) { s.running = true; s.focused = false; s.occluded = true; })},
        {"TV playing, covered window", st([](pace::State& s) { s.running = s.video = true; s.focused = false; s.occluded = true; })},
        {"dragging, on battery", st([](pace::State& s) { s.recentInput = true; s.onBattery = true; })},
        {"receiving, background, battery", st([](pace::State& s) { s.running = true; s.focused = false; s.onBattery = true; })},
        {"TV playing, on battery", st([](pace::State& s) { s.running = s.video = true; s.onBattery = true; })},
        {"minimised", st([](pace::State& s) { s.shown = false; s.running = true; })},
    };
    printf("%-34s %10s %10s\n", "window state", "before fps", "after fps");
    for (const Row& r : rows) printf("%-34s %10.1f %10.1f\n", r.name, 1 / legacyInterval(r.s), 1 / pace::interval(r.s));
}

// a radio that writes `chunk` samples `perSec` times a second; the consumer loops the way Engine::analysisLoop() does
static void ringCase(const char* name, double rate, size_t chunk, bool useWait, double seconds) {
    auto ring = std::make_unique<dect2::IqRingCpp>(1u << 22);
    std::atomic<bool> stop{false};
    std::vector<cf32> src(chunk, cf32(0.1f, 0.f));
    std::thread prod([&] {
        if (rate <= 0) { while (!stop) std::this_thread::sleep_for(milliseconds(5)); return; }   // a stalled radio
        const auto period = duration<double>(chunk / rate);
        auto t = steady_clock::now();
        while (!stop) {
            t += duration_cast<steady_clock::duration>(period);
            std::this_thread::sleep_until(t);
            ring->write(src.data(), chunk);
        }
    });
    std::vector<cf32> buf(1 << 16);
    long wakes = 0;
    size_t got = 0;
    const double c0 = threadCpu();
    const auto t0 = steady_clock::now();
    const auto end = t0 + duration_cast<steady_clock::duration>(duration<double>(seconds));
    const size_t minN = (size_t)std::clamp(rate * 0.005, 1.0, (double)buf.size());
    auto next = t0;
    while (steady_clock::now() < end) {
        while (size_t n = ring->read(buf.data(), buf.size())) got += n;
        next += milliseconds(33);
        for (;;) {
            const auto now = steady_clock::now();
            if (now >= next) break;
            const size_t m = ring->read(buf.data(), buf.size());
            if (m) { got += m; continue; }
            if (useWait) ring->waitData(minN, duration_cast<microseconds>(next - now));
            else std::this_thread::sleep_for(milliseconds(2));
            wakes++;
        }
    }
    const double cpu = threadCpu() - c0, el = duration<double>(steady_clock::now() - t0).count();
    stop = true;
    prod.join();
    printf("%-30s %-10s %10.0f %10.3f %12.1f\n", name, useWait ? "waitData" : "sleep 2ms", wakes / el, 100 * cpu / el, got / el / 1e6);
}

static void ring() {
    printf("%-30s %-10s %10s %10s %12s\n", "radio", "consumer", "wakeups/s", "cpu %", "Msps read");
    struct C { const char* name; double rate; size_t chunk; } cs[] = {
        {"stalled (no samples)", 0, 1},
        {"sound card 48 kHz, 1024", 48e3, 1024},
        {"RTL-SDR 2.4 Msps, 16384", 2.4e6, 16384},
        {"HackRF 10 Msps, 131072", 10e6, 131072},
        {"SDRplay 2 Msps, 1008", 2e6, 1008},
    };
    for (const C& c : cs) { ringCase(c.name, c.rate, c.chunk, false, 2.0); ringCase(c.name, c.rate, c.chunk, true, 2.0); }
}

static void trBench() {
    const char* keys[] = {"Start", "Stop", "Frequency", "Gain##rx", "Sample rate##src", "Not a catalog key at all", "Waterfall##wf2", "Record", "Reduce CPU when hidden##viewmenu"};
    const int K = sizeof keys / sizeof *keys, N = 2000000;
    printf("%-10s %12s %16s\n", "language", "ns per tr()", "allocs per 1000");
    for (const char* lang : {"en", "de", "ja"}) {
        i18n::set(lang);
        size_t sink = 0;
        for (int i = 0; i < 1000; i++) sink += (size_t)tr(keys[i % K]);   // warm the caches
        const long a0 = gAllocs.load();
        const auto t0 = steady_clock::now();
        for (int i = 0; i < N; i++) sink += (size_t)tr(keys[i % K]);
        const double ns = duration<double, std::nano>(steady_clock::now() - t0).count() / N;
        printf("%-10s %12.1f %16.1f%s\n", lang, ns, 1000.0 * (gAllocs.load() - a0) / N, sink == 1 ? " " : "");
    }
    i18n::set("en");
}

static void rfcam() {
    using namespace dect2::df;
    View v; v.w = 1080; v.h = 270; v.proj = Projection::Equirect; v.hfovDeg = 360;
    std::vector<uint32_t> base((size_t)v.w * v.h, 0xFF404040u), comp;
    std::vector<double> bearings, spectrum;
    for (int i = 0; i < 360; i++) { bearings.push_back(i - 180); spectrum.push_back(20 * std::exp(-0.01 * (i - 90) * (i - 90))); }
    const int N = 200;
    const double c0 = threadCpu();
    for (int i = 0; i < N; i++) { comp = base; blendHeat(comp, v, bearings, spectrum, {}); }
    const double ms = 1e3 * (threadCpu() - c0) / N;
    const double mb = v.w * v.h * 4 / 1e6;   // one texture upload
    printf("%-34s %10s %12s %14s\n", "RF camera picture (1080x270)", "ms/blend", "CPU % core", "upload MB/s");
    printf("%-34s %10.2f %12.2f %14.1f\n", "before: every frame at 60 fps", ms, 60 * ms / 10, 60 * mb);
    printf("%-34s %10.2f %12.2f %14.1f\n", "after: on a new result (8 Hz)", ms, 8 * ms / 10, 8 * mb);
}

int main(int argc, char** argv) {
    auto want = [&](const char* s) { if (argc < 2) return true; for (int i = 1; i < argc; i++) if (!strcmp(argv[i], s)) return true; return false; };
    if (want("frames")) { frames(); printf("\n"); }
    if (want("tr")) { trBench(); printf("\n"); }
    if (want("rfcam")) { rfcam(); printf("\n"); }
    if (want("ring")) ring();
    return 0;
}
