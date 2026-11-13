// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The DJI polyphase resampler computes four outputs per pass; every output must equal the single-output computation bit for bit,
// including the outputs whose window leaves the input (zero padded) at both ends. A one-output run takes the single path, which is the reference.
#include "dect2/dji/dji_dsp.h"
#include "dect2/common/stable_random.h"
#include <cstdio>
#include <cstring>
#include <random>

using namespace dect2;

int main() {
    int fails = 0;
    std::mt19937 rng(7);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    struct Case { double in, out, cut; size_t n; double start; } cases[] = {
        {20e6, 15.36e6, 7e6, 40000, 0.0}, {20e6, 30.72e6, 9.2e6, 40000, 0.37}, {10e6, 15.36e6, 4.6e6, 5003, -5.5}, {8e6, 30.72e6, 3.6e6, 3000, 2.9}};
    for (const Case& c : cases) {
        std::vector<cf32> x(c.n);
        for (auto& v : x) v = cf32{nd(rng), nd(rng)};
        dji::Resampler rs(c.in, c.out, c.cut);
        const size_t nOut = (size_t)((double)c.n * c.out / c.in) + 37;   // runs past the end of the input as well
        const auto got = rs.run(x.data(), x.size(), c.start, nOut);
        // a one-output run at the same position takes the single path
        const double step = c.in / c.out;
        size_t bad = 0;
        for (size_t m = 0; m < nOut; m += 1 + (m % 7)) {
            const auto one = rs.run(x.data(), x.size(), c.start + (double)m * step, 1);
            if (std::memcmp(&one[0], &got[m], sizeof(cf32)) != 0) bad++;
        }
        if (bad) { printf("FAIL: %zu of the sampled outputs differ from the single-output path (%.0f -> %.0f)\n", bad, c.in, c.out); fails++; }
    }
    if (!fails) printf("dji resampler: ok\n");
    return fails ? 1 : 0;
}
