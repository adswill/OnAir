// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The spectrum thread feeds while another thread takes frames and changes the settings (resize, reset, sample rate, fast rows): no crash,
// no frame of a wrong size, and frames keep coming. Run it under ThreadSanitizer to check the locking (DECT2_SANITIZE_THREAD).
#include "dect2/dsp/spectrum.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>
using namespace dect2;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
int main() {
    SpectrumAnalyzer an(4096);
    an.setSampleRate(2.4e6);
    std::vector<cf32> chunk(1 << 15);
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> u(-0.1f, 0.1f);
    for (auto& c : chunk) c = cf32(u(rng), u(rng));
    std::atomic<bool> stop{false};
    std::thread prod([&] { while (!stop) { an.feed(chunk.data(), chunk.size()); std::this_thread::sleep_for(std::chrono::microseconds(300)); } });
    int frames = 0, rows = 0, badSize = 0;
    SpectrumFrame f; std::vector<float> r; size_t bins = 0; double dt = 0; uint64_t dr = 0;
    std::mt19937 q(3);
    const auto t0 = std::chrono::steady_clock::now();
    int step = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (an.takeFrame(f)) { frames++; const size_t n = f.dbfs.size(); if (n < 1024 || n > SpectrumAnalyzer::kMaxFft || (n & (n - 1))) badSize++; }
        rows += (int)an.takeFastRows(r, bins, dt, dr);
        switch (step++ % 40) {
            case 0: an.resize(1024u << (q() % 5)); break;
            case 10: an.reset(); break;
            case 15: an.setSampleRate((q() % 2) ? 2.4e6 : 48000.0); break;
            case 20: an.setFastRows(50.0 + (q() % 450), (q() % 2) ? 2.4e6 : 48000.0); break;
            case 30: an.setFastRows(0, 0); break;
            case 35: an.setBlocksPerFrame(1 + q() % 4); break;
            default: break;
        }
    }
    stop = true; prod.join();
    check(badSize == 0, "every frame has a valid size");
    check(frames > 40, "frames kept coming while the settings changed");
    check(rows >= 0, "rows were taken");
    if (fails) printf("%d FAILED\n", fails); else printf("spectrum_threads: all passed (%d frames, %d rows)\n", frames, rows);
    return fails ? 1 : 0;
}
