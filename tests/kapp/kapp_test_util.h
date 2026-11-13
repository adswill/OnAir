// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared helpers of the KAPP tests: seeded random draws (kris XorShift64, Box-Muller) and a channel (noise, carrier offset, start offset).
#pragma once
#include "dect2/kapp/kapp_arq.h"
#include "dect2/kapp/kapp_frame.h"
#include "dect2/kapp/kapp_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kapptest {
using dect2::kapp::cf;

struct Rng {
    dect2::kris::XorShift64 x;
    bool have = false;
    double spare = 0.0;
    explicit Rng(uint64_t seed) : x{seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull} {}
    double uniform() { return (double)(x.next() >> 11) * (1.0 / 9007199254740992.0); }
    uint64_t below(uint64_t n) { return x.next() % n; }
    double gauss() {
        if (have) {
            have = false;
            return spare;
        }
        double u1 = uniform();
        const double u2 = uniform();
        if (u1 < 1e-300) u1 = 1e-300;
        const double r = std::sqrt(-2.0 * std::log(u1));
        const double a = 6.283185307179586 * u2;
        spare = r * std::sin(a);
        have = true;
        return r * std::cos(a);
    }
};

inline std::vector<uint8_t> randomBytes(Rng& r, size_t n) {
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = (uint8_t)r.below(256);
    return v;
}

// Es/N0 in dB: the burst has unit average power per sample and 8 samples per symbol, so the complex noise variance per sample is 8 / EsN0.
// esn0Db > 200 means no noise. Order of the draws is fixed by the braced initialiser (left to right).
inline std::vector<cf> channel(const std::vector<cf>& burst, double esn0Db, double cfoHz, Rng& r, size_t lead, size_t tail) {
    std::vector<cf> out(lead + burst.size() + tail);
    const double sg = esn0Db > 200.0 ? 0.0 : std::sqrt(8.0 / std::pow(10.0, esn0Db / 10.0) / 2.0);
    const double ph0 = 6.283185307179586 * r.uniform();
    for (size_t i = 0; i < out.size(); ++i) {
        cf v{0.f, 0.f};
        if (sg > 0.0) v = cf{(float)(sg * r.gauss()), (float)(sg * r.gauss())};
        if (i >= lead && i < lead + burst.size()) {
            const double ph = ph0 + 6.283185307179586 * cfoHz * (double)(i - lead) / dect2::kapp::kInternalRate;
            v += burst[i - lead] * cf{(float)std::cos(ph), (float)std::sin(ph)};
        }
        out[i] = v;
    }
    return out;
}

inline std::vector<cf> noiseOnly(size_t n, Rng& r) {
    std::vector<cf> out(n);
    const double sg = std::sqrt(0.5);
    for (auto& v : out) v = cf{(float)(sg * r.gauss()), (float)(sg * r.gauss())};
    return out;
}

inline void feedChunks(dect2::kapp::BurstReceiver& rx, const std::vector<cf>& x, size_t chunk) {
    for (size_t i = 0; i < x.size(); i += chunk) rx.feed(x.data() + i, std::min(chunk, x.size() - i));
}

}  // namespace kapptest

static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)
