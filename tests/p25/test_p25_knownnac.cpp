// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// P25 frame header with the site's NAC known: how many bit errors it survives, against the BCH decoder, and how often noise is taken for a frame. Simulated bits.
#include <vector>
#include "dect2/p25/p25_proto.h"
#include <cstdio>
#include <random>
using namespace dect2;
#include "../common/test_util.h"
int main() {
    std::mt19937 r(5);
    for (int errs : {8, 12, 14, 16, 18}) {
        int bch = 0, known = 0, wrong = 0; const int N = 3000;
        for (int k = 0; k < N; k++) {
            const int duid = (k & 1) ? 5 : 10;
            uint8_t b[64]; p25::nidEncode(0x293, duid, b);
            std::vector<int> pos; while ((int)pos.size() < errs) { int p = r() % 64; bool dup = false; for (int q : pos) dup |= q == p; if (!dup) pos.push_back(p); }
            for (int p : pos) b[p] ^= 1;
            int n = 0, d = 0;
            if (p25::nidDecode(b, n, d) && n == 0x293 && d == duid) bch++;
            d = -1;
            if (p25::nidDecodeKnownNac(b, 0x293, d)) { if (d == duid) known++; else wrong++; }
        }
        printf("%2d bit errors: BCH %.1f%%, known NAC %.1f%%, wrong type %.2f%%\n", errs, 100.0 * bch / N, 100.0 * known / N, 100.0 * wrong / N);
        if (errs >= 14 && errs <= 16) CHECK(known > bch, "gain at %d", errs);
        CHECK(wrong < N / 100, "wrong type at %d: %d", errs, wrong);
    }
    int false0 = 0; const int M = 200000;
    for (int k = 0; k < M; k++) { uint8_t b[64]; for (auto& x : b) x = r() & 1; int d = 0; if (p25::nidDecodeKnownNac(b, 0x293, d)) false0++; }
    printf("noise taken for a frame: %d of %d\n", false0, M);
    CHECK(false0 < M / 200, "false alarms %d", false0);
    return testSummary("p25_knownnac");
}
