// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Where the time goes for a big spectrum FFT (131072, 262144 ...): the engine side (window, FFT, power, dB conversion in takeFrame) and the
// per-frame passes of the app (copies, trace and waterfall averaging, history levels). Usage: bench_spectrum [fs_Msps] [seconds]
// Prints ms per spectrum frame for each part and the CPU share at the frame rate the sample rate allows (max 30 a second).
#include "dect2/dsp/spectrum.h"
#include "dect2/dsp/dsp_compat.h"
#include "gfx/spec_pipe.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <random>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>

using namespace dect2;
using Clock = std::chrono::steady_clock;
static double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }


// How long the analysis thread waits in takeFrame while the spectrum thread is busy with a transform (the analysis thread is the decoders'
// thread: it must not wait). A producer feeds samples flat out, a consumer takes a frame every 33 ms and times the call.
static void contention(double fs, size_t n, bool setRate) {
    SpectrumAnalyzer an(n);
#ifndef BENCH_OLD_ANALYZER   // (-DBENCH_OLD_ANALYZER: built against the analyser as it was before 3.3, for the before / after table)
    if (setRate) an.setSampleRate(fs);
#else
    (void)setRate;
#endif
    std::vector<cf32> chunk(1 << 16);
    std::mt19937 rng(2);
    std::normal_distribution<float> nd(0.f, 0.05f);
    for (auto& c : chunk) c = cf32(nd(rng), nd(rng));
    std::atomic<bool> stop{false};
    std::thread prod([&] {   // paced like a radio: a piece every 65536 / fs seconds (flat out only when the machine cannot keep up)
        auto due = Clock::now();
        while (!stop) {
            an.feed(chunk.data(), chunk.size());
            due += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(chunk.size() / fs));
            std::this_thread::sleep_until(due);
        }
    });
    std::vector<double> waits;
    int got = 0;
    SpectrumFrame f;
    const auto t0 = Clock::now();
    while (since(t0) < 3.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        const auto a = Clock::now();
        const bool ok = an.takeFrame(f);
        waits.push_back(1e3 * since(a));
        got += ok;
    }
    stop = true; prod.join();
    std::sort(waits.begin(), waits.end());
    printf("contention N=%7zu%s: takeFrame wait avg %.3f ms, p95 %.3f, max %.3f ms; frames %d in 3 s\n", n, setRate ? " (rate set)" : "", [&] { double t = 0; for (double w : waits) t += w; return t / waits.size(); }(),
           waits[waits.size() * 95 / 100], waits.back(), got);
}

// Frames per second of signal the display gets (the analysis thread takes a frame every 1/30 s of signal) and the CPU per second of signal.
static void framerate(double fs, size_t n, bool setRate) {
    SpectrumAnalyzer an(n);
#ifndef BENCH_OLD_ANALYZER
    if (setRate) an.setSampleRate(fs);
#else
    (void)setRate;
#endif
    std::vector<cf32> chunk((size_t)(fs / 30));
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.f, 0.05f);
    for (auto& c : chunk) c = cf32(nd(rng), nd(rng));
    SpectrumFrame f;
    int frames = 0;
    const int periods = 90;   // 3 s of signal
    const auto t0 = Clock::now();
    for (int i = 0; i < periods; i++) { an.feed(chunk.data(), chunk.size()); frames += an.takeFrame(f); }
    const double cpu = since(t0);
    printf("framerate fs %4.1f Msps N=%7zu%s: %5.1f frames/s, %5.1f ms CPU per second of signal\n", fs / 1e6, n, setRate ? " (rate set)" : "", frames / 3.0, 1e3 * cpu / 3.0);
}

#ifndef BENCH_OLD_ANALYZER
// The CPU the fast waterfall rows cost on top of the ordinary frames: CPU per second of signal with rows off and on.
static void fastRowCost(double fs, double rps) {
    std::vector<cf32> chunk((size_t)(fs / 30));
    std::mt19937 rng(4);
    std::normal_distribution<float> nd(0.f, 0.05f);
    for (auto& c : chunk) c = cf32(nd(rng), nd(rng));
    double cpu[2] = {0, 0}; size_t rows = 0, bins = 0;
    for (int on = 0; on < 2; on++) {
        SpectrumAnalyzer an(8192);
        an.setSampleRate(fs);
        if (on) an.setFastRows(rps, fs);
        SpectrumFrame f; std::vector<float> r; double dt = 0; uint64_t dr = 0;
        const auto t0 = Clock::now();
        for (int i = 0; i < 90; i++) { an.feed(chunk.data(), chunk.size()); an.takeFrame(f); if (on) rows += an.takeFastRows(r, bins, dt, dr); }
        cpu[on] = since(t0) / 3.0;
    }
    printf("fast rows fs %5.2f Msps, %3.0f rows/s: %6zu-point transforms, %5.0f rows/s made, +%5.1f ms CPU per second of signal (%.1f%% of a core)\n", fs / 1e6, rps, bins, rows / 3.0,
           1e3 * (cpu[1] - cpu[0]), 100 * (cpu[1] - cpu[0]));
}
#endif

int main(int argc, char** argv) {
    const double fs = (argc > 1 ? atof(argv[1]) : 10.0) * 1e6;
    const double secs = argc > 2 ? atof(argv[2]) : 3.0;
    std::mt19937 rng(1);
    std::normal_distribution<float> nd(0.f, 0.05f);
    std::vector<cf32> chunk(1 << 16);
    for (auto& c : chunk) c = cf32(nd(rng), nd(rng));
    printf("fs %.1f Msps. ms per frame: feed (window+FFT+power), takeFrame (dB), copy x2, trace avg, row avg, levels, total; frames/s possible; cpu%% of one core at that rate\n", fs / 1e6);
    for (size_t n : {4096u, 16384u, 65536u, 131072u, 262144u}) {
        SpectrumAnalyzer an(n);
        an.setBlocksPerFrame(SpectrumAnalyzer::kDefaultBlocks);
        SpectrumFrame f, g, h;
        double tFeed = 0, tTake = 0; int frames = 0;
        const size_t perFrame = n * SpectrumAnalyzer::kDefaultBlocks;
        const auto t0 = Clock::now();
        while (since(t0) < secs) {
            size_t fed = 0;
            auto a = Clock::now();
            while (fed < perFrame) { an.feed(chunk.data(), std::min(chunk.size(), perFrame - fed)); fed += std::min(chunk.size(), perFrame - fed); }
            tFeed += since(a);
            a = Clock::now();
            an.takeFrame(f);
            tTake += since(a);
            frames++;
        }
        // the app side
        std::vector<float> view(n), smooth(n), pw, rowAvg(n), rowPw;
        std::vector<uint8_t> lv(65536);
        const int C = (int)std::min<size_t>(n, 65536);
        double tCopy = 0, tTr = 0, tRow = 0, tLev = 0; const int reps = 30;
        for (int r = 0; r < reps; r++) {
            auto a = Clock::now();
            g = f; h = g;                        // engine's spec_ = f, latestSpectrum's out = spec_
            tCopy += since(a); a = Clock::now();
            for (size_t i = 0; i < n; i++) view[i] = h.dbfs[i] - 3.f;
            tCopy += since(a); a = Clock::now();
            spipe::powerEma(pw, view.data(), smooth.data(), n, 0.2f);
            tTr += since(a); a = Clock::now();
            spipe::powerEma(rowPw, view.data(), rowAvg.data(), n, 0.2f);
            tRow += since(a); a = Clock::now();
            spipe::levelsRow(rowAvg.data(), n, C, -100.f, -40.f, lv.data());
            tLev += since(a);
        }
        const double ms = 1e3 * tFeed / frames, mt = 1e3 * tTake / frames, mc = 1e3 * tCopy / reps, mtr = 1e3 * tTr / reps, mr = 1e3 * tRow / reps, ml = 1e3 * tLev / reps;
        const double total = ms + mt + mc + mtr + mr + ml;
        const double possible = fs / (double)perFrame;           // frames/s the sample rate gives
        const double rate = std::min(30.0, possible);
        printf("N=%7zu feed %7.3f take %6.3f copy %6.3f trace %6.3f row %6.3f lev %6.3f | total %7.3f ms | %5.1f fps -> %5.1f%% core (engine %5.1f%%, ui %5.1f%%)\n", n, ms, mt, mc, mtr, mr, ml, total,
               rate, 100 * rate * total / 1e3, 100 * rate * (ms + mt) / 1e3, 100 * rate * (mc + mtr + mr + ml) / 1e3);
    }
    for (size_t n : {16384u, 65536u, 131072u, 262144u}) {
        framerate(fs, n, false);
#ifndef BENCH_OLD_ANALYZER
        framerate(fs, n, true);
#endif
    }
#ifndef BENCH_OLD_ANALYZER
    for (double f : {0.048e6, 2.4e6, 10e6}) for (double r : {100.0, 200.0, 500.0}) fastRowCost(f, r);
#endif
    for (size_t n : {65536u, 262144u}) {
        contention(fs, n, false);
#ifndef BENCH_OLD_ANALYZER
        contention(fs, n, true);
#endif
    }
    return 0;
}
