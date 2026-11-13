// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The engine's input guard (dropNonFinite, sample_convert.h): NaN, infinity and magnitudes of 2^20 and more become silence, whole samples;
// every value a radio can deliver stays bit for bit (full scale 1, unscaled 16-bit 32768, tiny and denormal values, negative zero).
// Samples of 1e30 once reached the GGAK timing loop as infinity and a float-to-int conversion (UBSan, fuzz_engine_modes).
#include "dect2/dsp/sample_convert.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>
#include "../common/test_util.h"
using namespace dect2;

int main() {
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    const float bad[] = {inf, -inf, nan, 1e30f, -3.4e38f, 1048576.f, -2e6f};
    const float good[] = {1.f, -1.f, 32768.f, -32767.f, 1048575.f, -1e5f, 1e-40f, -0.f, 0.f, 3e-8f};
    std::vector<cf32> v;
    for (float b : bad) for (float g : good) { v.push_back(cf32(b, g)); v.push_back(cf32(g, b)); }
    for (float g : good) for (float h : good) v.push_back(cf32(g, h));
    std::vector<cf32> in = v;
    dropNonFinite(v.data(), v.size());
    const size_t nBad = 2 * (sizeof bad / sizeof bad[0]) * (sizeof good / sizeof good[0]);
    for (size_t i = 0; i < v.size(); i++) {
        if (i < nBad) CHECK(v[i] == cf32(0, 0), "sample %zu (%g, %g) not silenced", i, in[i].real(), in[i].imag());
        else CHECK(!memcmp(&v[i], &in[i], sizeof(cf32)), "sample %zu (%g, %g) changed", i, in[i].real(), in[i].imag());
    }
    // blocks: one bad value in a long run of good ones, at every position of a 256-value block and across the block edge
    for (size_t pos : {0, 1, 127, 128, 255, 256, 1000}) {
        std::vector<cf32> w(700, cf32(0.5f, -0.25f));
        w[pos % w.size()] = cf32(0.5f, 1e31f);
        dropNonFinite(w.data(), w.size());
        size_t zeros = 0;
        for (auto& x : w) zeros += x == cf32(0, 0);
        CHECK(zeros == 1 && w[pos % w.size()] == cf32(0, 0), "pos %zu: %zu silenced", pos, zeros);
    }
    return testSummary("sample_guard");
}
