// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The vector Viterbi decoder (fecp::viterbiRange: NEON, SSE2 or AVX2) against a plain scalar copy of the same trellis: the same decided bits and
// the same final path metric on coded data with noise, punctured (zero) soft values, pure noise and full-scale values, at chunk sizes the receivers use.
#include "dect2/fec/fec_proto.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace dect2;

namespace {

// the scalar K = 7 decoder (generators 171 / 133 octal), int32 metrics renormalised every step
void reference(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metric) {
    int sx[64][2], sy[64][2];
    for (int ns = 0; ns < 64; ns++)
        for (int b = 0; b < 2; b++) {
            const unsigned reg = ((unsigned)(ns >> 5) << 6) | (unsigned)(((ns & 31) << 1) | b);
            sx[ns][b] = __builtin_parity(reg & 0x79) ? -1 : 1;
            sy[ns][b] = __builtin_parity(reg & 0x5B) ? -1 : 1;
        }
    std::vector<uint64_t> dec(steps);
    long off = 0;
    int pm[64] = {}, nm[64];
    for (size_t t = 0; t < steps; t++) {
        const int a = soft[2 * t], c = soft[2 * t + 1];
        uint64_t d = 0;
        for (int ns = 0; ns < 64; ns++) {
            const int p0 = (ns & 31) << 1, p1 = p0 | 1;
            const int m0 = pm[p0] + sx[ns][0] * a + sy[ns][0] * c, m1 = pm[p1] + sx[ns][1] * a + sy[ns][1] * c;
            if (m1 > m0) { nm[ns] = m1; d |= 1ull << ns; } else nm[ns] = m0;
        }
        int best = nm[0];
        for (int s = 1; s < 64; s++) best = std::max(best, nm[s]);
        off += best;
        for (int s = 0; s < 64; s++) pm[s] = nm[s] - best;
        dec[t] = d;
    }
    int state = 0, bm = pm[0];
    for (int s = 1; s < 64; s++) if (pm[s] > bm) { bm = pm[s]; state = s; }
    bits.assign(steps, 0);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(state >> 5);
        state = ((state & 31) << 1) | (int)((dec[t] >> state) & 1);
    }
    *metric = off + bm;
}

std::vector<int8_t> coded(size_t steps, double noise, double punct, int amp, std::mt19937& rng) {
    std::vector<int8_t> s(2 * steps);
    std::normal_distribution<double> nd(0, 1);
    std::uniform_real_distribution<double> u(0, 1);
    unsigned reg = 0;
    for (size_t t = 0; t < steps; t++) {
        reg = ((reg << 1) | (rng() & 1)) & 0x7F;
        const int x = __builtin_parity(reg & 0x79), y = __builtin_parity(reg & 0x5B);   // the same taps as the decoder's trellis
        for (int k = 0; k < 2; k++) {
            const int bit = k ? y : x;
            double v = (bit ? -amp : amp) + noise * amp * nd(rng);
            if (u(rng) < punct) v = 0;
            s[2 * t + k] = (int8_t)std::clamp((int)std::lround(v), -127, 127);
        }
    }
    return s;
}

} // namespace

int main() {
    std::mt19937 rng(7);
    struct Case { size_t steps; double noise, punct; int amp; } cases[] = {
        {16384 + 384, 0.3, 0.0, 22}, {16384 + 384, 0.8, 0.25, 22}, {5000, 2.0, 0.0, 22}, {777, 0.5, 0.4, 127}, {100000, 0.6, 0.1, 40}, {3, 0.1, 0, 22}, {1, 0, 0, 22},
    };
    int fails = 0;
    for (const Case& c : cases) {
        for (int rep = 0; rep < 3; rep++) {
            const auto soft = coded(c.steps, c.noise, c.punct, c.amp, rng);
            std::vector<uint8_t> a, b;
            long ma = 0, mb = 0;
            fecp::viterbiRange(soft.data(), c.steps, a, &ma);
            reference(soft.data(), c.steps, b, &mb);
            if (a != b || ma != mb) {
                size_t first = 0;
                while (first < a.size() && first < b.size() && a[first] == b[first]) first++;
                printf("FAIL steps %zu noise %.1f punct %.2f amp %d: metric %ld vs %ld, first differing bit %zu\n", c.steps, c.noise, c.punct, c.amp, ma, mb, first);
                fails++;
            }
        }
    }
    printf(fails ? "viterbi simd: %d failures\n" : "viterbi simd: ok\n", fails);
    return fails ? 1 : 0;
}
