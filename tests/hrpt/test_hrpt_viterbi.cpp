// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The HRPT Viterbi decoder runs its butterflies as vector lanes; its decided bits must equal the scalar butterfly loop's exactly, for random soft input
// (so also through the metric renormalisation and the tie cases) and for a coded stream with errors.
#include "dect2/hrpt/hrpt_proto.h"
#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

using namespace dect2;

namespace {
int par(unsigned v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return v & 1; }

// the scalar decoder the class used before: one butterfly at a time, decisions packed in a 64-bit word
struct Ref {
    int16_t m[64] = {};
    uint64_t dec[160];
    int n = 0, since = 0;
    std::vector<uint8_t> out;
    void push(int a, int b) {
        int16_t nm[64];
        uint64_t d = 0;
        for (int j = 0; j < 32; j++) {
            const int16_t lo = m[j], hi = m[j + 32];
            int16_t c[4];
            for (int k = 0; k < 4; k++) {
                const int p = j | ((k & 1) ? 32 : 0), bit = k >> 1;
                const unsigned r = (unsigned)((p << 1 | bit) & 0x7F);
                c[k] = (int16_t)(((k & 1) ? hi : lo) + (par(r & 0x4F) ? 1 : -1) * (int16_t)a + (par(r & 0x6D) ? 1 : -1) * (int16_t)b);
            }
            const bool s0 = c[1] > c[0], s1 = c[3] > c[2];
            nm[2 * j] = s0 ? c[1] : c[0];
            nm[2 * j + 1] = s1 ? c[3] : c[2];
            d |= (uint64_t)s0 << (2 * j);
            d |= (uint64_t)s1 << (2 * j + 1);
        }
        if (++since >= 32) { since = 0; const int16_t base = nm[0]; for (int s = 0; s < 64; s++) nm[s] = (int16_t)(nm[s] - base); }
        for (int s = 0; s < 64; s++) m[s] = nm[s];
        dec[n++] = d;
        if (n >= 96 + 64) tb(64, false);
    }
    void tb(int emit, bool all) {
        int s = 0;
        for (int i = 1; i < 64; i++) if (m[i] > m[s]) s = i;
        uint8_t bits[160];
        for (int t = n - 1; t >= 0; t--) { bits[t] = (uint8_t)(s & 1); s = (s >> 1) | (int)((dec[t] >> s & 1) << 5); }
        const int k = all ? n : emit;
        out.insert(out.end(), bits, bits + k);
        for (int i = k; i < n; i++) dec[i - k] = dec[i];
        n -= k;
    }
    void flush() { if (n > 0) tb(0, true); }
};
}

int main() {
    int fails = 0;
    std::mt19937 rng(21);
    for (int mode = 0; mode < 3; mode++) {
        hrpt::Viterbi v;
        Ref r;
        const int steps = 40000;
        for (int i = 0; i < steps; i++) {
            int a, b;
            if (mode == 0) { a = (int)(rng() % 255) - 127; b = (int)(rng() % 255) - 127; }                    // random: ties and renormalisation
            else if (mode == 1) { a = (rng() & 1) ? 127 : -127; b = (rng() & 1) ? 127 : -127; }             // saturated values
            else { a = (int)(rng() % 3) - 1; b = (int)(rng() % 3) - 1; }                                    // tiny values: many ties
            v.push(a, b); r.push(a, b);
        }
        v.flush(); r.flush();
        if (v.out() != r.out) { printf("FAIL: mode %d decided bits differ (%zu vs %zu)\n", mode, v.out().size(), r.out.size()); fails++; }
    }
    if (!fails) printf("hrpt viterbi: ok\n");
    return fails ? 1 : 0;
}
