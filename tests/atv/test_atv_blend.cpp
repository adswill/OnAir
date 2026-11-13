// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analogue TV temporal noise reduction: the table-driven blend must give the same bytes as the per-byte reference, for every colour pair.
#include "dect2/atv/atv_blend.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace dect2;

static uint8_t reference(int c, int p, double k, double T) {
    const double d = std::abs(c - p);
    const double w = d <= T ? k : std::min(1.0, k + (d - T) / T);
    return (uint8_t)std::lround(p + (c - p) * w);
}

int main() {
    int bad = 0;
    const double ks[] = {0.12, 0.15, 0.3, 0.5, 0.77, 0.998};
    const double sigs[] = {0.0, 0.003, 0.02, 0.05, 0.2, 1.0};
    for (double k : ks) for (double sg : sigs) {
        const double T = 5.0 * sg * 255.0 + 8.0;
        std::vector<uint8_t> cur(256 * 256 * 4), prev(256 * 256 * 4), want(256 * 256 * 4);
        for (int c = 0; c < 256; c++) for (int p = 0; p < 256; p++) {
            const size_t i = ((size_t)c * 256 + (size_t)p) * 4;
            for (int j = 0; j < 4; j++) { cur[i + j] = (uint8_t)c; prev[i + j] = (uint8_t)p; want[i + j] = j < 3 ? reference(c, p, k, T) : (uint8_t)c; }
        }
        atvTemporalBlend(cur.data(), prev.data(), cur.size(), k, T);
        for (size_t i = 0; i < cur.size(); i++) if (cur[i] != want[i]) { if (bad++ < 5) std::printf("FAIL k=%g sig=%g byte %zu: got %d want %d\n", k, sg, i, cur[i], want[i]); }
    }
    std::printf(bad ? "FAIL: %d bytes differ\n" : "ok: blend equals the reference on all inputs\n", bad);
    return bad ? 1 : 0;
}
