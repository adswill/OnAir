// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Burst detector on a generated tone burst: power per 1000-sample block of a complex tone over noise. One 60-block burst is found once with
// its start, length and levels; a 2-block spike is too short to count; a burst still open at the end of the input is flushed; the baseline
// is not lifted by the burst itself (it is longer than the median window).
#include "dect2/specan/burst_monitor.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const double fs = 100e3, dtMs = 10.0;   // 1000 samples per block
    const size_t block = 1000, nBlocks = 800;
    const double kPi = 3.14159265358979323846;
    std::mt19937 rng(7);
    std::normal_distribution<float> gauss(0.f, 0.01f / std::sqrt(2.f));
    auto on = [](size_t b) { return (b >= 300 && b < 360) || b == 500 || b == 501; };
    std::vector<float> db(nBlocks);
    for (size_t b = 0; b < nBlocks; b++) {
        double p = 0;
        for (size_t k = 0; k < block; k++) {
            const size_t n = b * block + k;
            std::complex<float> x(gauss(rng), gauss(rng));
            if (on(b)) x += std::polar(0.5f, (float)(2 * kPi * 5e3 * (double)n / fs));
            p += std::norm(x);
        }
        db[b] = (float)(10 * std::log10(p / block));
    }
    BurstConfig cfg;
    cfg.riseDb = 10;
    cfg.minMs = 100;
    cfg.medianWindow = 15;
    {   // whole series: one burst, 60 blocks at 10 ms from 3000 ms, level about -6 dB over about -40 dB
        auto ev = detectBursts(db, dtMs, cfg);
        CHECK(ev.size() == 1, "events %zu", ev.size());
        if (ev.size() == 1) {
            CHECK(std::fabs(ev[0].startMs - 3000) < 1e-6, "start %.2f", ev[0].startMs);
            CHECK(std::fabs(ev[0].durationMs - 590) < 1e-6, "duration %.2f", ev[0].durationMs);
            CHECK(ev[0].peakDb > -8 && ev[0].peakDb < -4, "peak %.2f", ev[0].peakDb);
            CHECK(ev[0].baseDb > -43 && ev[0].baseDb < -37, "base %.2f", ev[0].baseDb);
            CHECK(ev[0].samples == 60, "samples %zu", ev[0].samples);
        }
    }
    {   // streaming gives the same burst
        BurstDetector d(cfg);
        BurstEvent ev;
        size_t got = 0;
        for (size_t i = 0; i < db.size(); i++) if (d.push(db[i], (double)i * dtMs, ev)) got++;
        CHECK(got == 1 && !d.inBurst(), "stream events %zu", got);
    }
    {   // a series that ends inside the burst is flushed
        std::vector<float> part(db.begin(), db.begin() + 330);
        auto ev = detectBursts(part, dtMs, cfg);
        CHECK(ev.size() == 1 && std::fabs(ev.empty() ? 0 : ev[0].durationMs - 290) < 1e-6, "flush events %zu", ev.size());
    }
    {   // the spike alone (2 blocks, 10 ms) is below the minimum length
        std::vector<float> spike(db.begin() + 490, db.begin() + 520);
        CHECK(detectBursts(spike, dtMs, cfg).empty(), "spike counted");
    }
    return testSummary("specan burst monitor");
}
