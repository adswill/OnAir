// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared helpers of the ham science tests: noise at a given SNR in 2500 Hz, simple check macro.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

static int g_fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); g_fails++; } } while (0)

struct TRng {
    uint64_t s;
    explicit TRng(uint32_t seed) : s(0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0xD1B54A32D192ED03ull)) { for (int i = 0; i < 4; i++) next(); }
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    double uni() { return ((next() >> 11) + 0.5) / 9007199254740992.0; }
    double gauss() { const double u = uni(), v = uni(); return std::sqrt(-2 * std::log(u)) * std::cos(6.283185307179586 * v); }
};

// y = x scaled so that its mean power (over samples where |x| > 0 is not assumed: over the whole vector) is sigPow, plus white noise of per-sample variance
// chosen so that the SNR in `bwHz` is snrDb (signal power = mean square of x * gain). Returns the noise sigma.
inline double addNoiseSnr(std::vector<float>& x, double fs, double snrDb, double bwHz, uint32_t seed, double sigPower) {
    TRng r(seed);
    const double nvar = sigPower / std::pow(10.0, snrDb / 10.0) * (fs / 2.0) / bwHz;
    const double sg = std::sqrt(nvar);
    for (float& v : x) v = (float)(v + sg * r.gauss());
    return sg;
}
inline std::vector<float> noiseOnly(size_t n, double sigma, uint32_t seed) {
    TRng r(seed);
    std::vector<float> y(n);
    for (float& v : y) v = (float)(sigma * r.gauss());
    return y;
}
