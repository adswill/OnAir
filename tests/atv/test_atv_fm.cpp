// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM-ATV generator (issue #584): the carrier keeps a constant envelope and its frequency follows the video: the sync tip is the lowest frequency,
// peak white the highest, the span between them the set deviation.
#include "dect2/atv/atv_gen.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s\n", msg); fails++; } } while (0)

int main() {
    AtvGenConfig c;
    c.rate = 24e6; c.cnrDb = 100; c.fmVideo = true; c.fmDevMhz = 8; c.pattern = 1;   // colour bars
    c.level = 0.4;
    AtvGenerator g(c);
    CHECK(g.ok(), "the generator starts in FM-ATV");
    std::vector<cf32> x;
    g.generate((size_t)(24e6 * 0.06), x);   // three frames
    double eMin = 1e9, eMax = 0;
    for (size_t i = 1000; i < x.size(); i++) { const double e = std::abs(x[i]); eMin = std::min(eMin, e); eMax = std::max(eMax, e); }
    CHECK(eMax - eMin < 0.02 * 0.4, "constant envelope");
    // instantaneous frequency (MHz) from the phase step, the products of 6 neighbouring samples added to smooth the edge rise
    double fLo = 1e9, fHi = -1e9;
    for (size_t i = 2000; i + 6 < x.size(); i += 6) {
        cf32 d(0, 0);
        for (int k = 0; k < 6; k++) d += x[i + k + 1] * std::conj(x[i + k]);
        const double f = std::arg(d) / (2 * M_PI) * c.rate / 1e6;
        fLo = std::min(fLo, f); fHi = std::max(fHi, f);
    }
    std::printf("FM-ATV frequency range %.2f .. %.2f MHz\n", fLo, fHi);
    CHECK(fHi - fLo > 6.5 && fHi - fLo < 9.5, "sync-to-white span is about the set deviation");
    CHECK(fLo < 0 && fHi > 0, "the sync tip is below the centre, white above it");
    std::printf(fails ? "atv_fm FAILED\n" : "atv_fm ok\n");
    return fails ? 1 : 0;
}
