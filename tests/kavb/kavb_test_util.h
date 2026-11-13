// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared helpers of the KAVB tests: seeded random draws, a channel (noise, carrier offset, phase, fractional delay, echo) and a one-frame run.
#pragma once
#include "dect2/kavb/kavb_phy.h"
#include "dect2/kavb/kavb_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kavbtest {
using dect2::kavb::cf;

struct Rng {
    dect2::kris::XorShift64 x;
    bool have = false;
    double spare = 0.0;
    explicit Rng(uint64_t seed) : x{seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull} {}
    double uniform() { return (double)(x.next() >> 11) * (1.0 / 9007199254740992.0); }
    uint64_t below(uint64_t n) { return x.next() % n; }
    double gauss() {
        if (have) { have = false; return spare; }
        double u1 = uniform();
        const double u2 = uniform();
        if (u1 < 1e-300) u1 = 1e-300;
        const double r = std::sqrt(-2.0 * std::log(u1)), a = 6.283185307179586 * u2;
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

struct Channel {
    double snrDb = 100.0;       // Es/N0 of the data (chirp not counted); 100 = no noise
    double cfoCycPerSym = 0.0;
    double phase = 0.0;
    double delaySamples = 0.0;  // fractional delay (cubic interpolation), 0 .. 1
    double echoAmp = 0.0;       // one echo, echoSyms symbols later
    double echoSyms = 1.0;
    double echoPhase = 0.7;
};

inline cf lag4(const std::vector<cf>& x, long i, double mu) {
    auto at = [&](long k) { return (k >= 0 && k < (long)x.size()) ? x[k] : cf(0, 0); };
    const float m = (float)mu;
    return at(i - 1) * (-m * (m - 1) * (m - 2) / 6.0f) + at(i) * ((m + 1) * (m - 1) * (m - 2) / 2.0f) + at(i + 1) * (-(m + 1) * m * (m - 2) / 2.0f) +
           at(i + 2) * ((m + 1) * m * (m - 1) / 6.0f);
}

// pad: samples of pure noise added in front and behind (so the receiver has to find the frame).
inline std::vector<cf> applyChannel(const std::vector<cf>& tx, const Channel& c, Rng& rng, size_t pad = 64) {
    std::vector<cf> x(tx.size() + 2 * pad, cf(0, 0));
    std::copy(tx.begin(), tx.end(), x.begin() + pad);
    std::vector<cf> y = x;
    if (c.delaySamples != 0.0) {
        const double d = c.delaySamples;
        for (long n = 0; n < (long)x.size(); n++) {
            const double pos = n - d;
            const long i = (long)std::floor(pos);
            y[n] = lag4(x, i, pos - i);
        }
    }
    if (c.echoAmp != 0.0) {
        const long e = (long)std::lround(c.echoSyms * dect2::kavb::kSps);
        const cf g = (float)c.echoAmp * cf((float)std::cos(c.echoPhase), (float)std::sin(c.echoPhase));
        std::vector<cf> z = y;
        for (long n = e; n < (long)y.size(); n++) z[n] += g * y[n - e];
        y = z;
    }
    const double sigma = std::sqrt(dect2::kavb::kSps / std::pow(10.0, c.snrDb / 10.0) / 2.0);
    for (long n = 0; n < (long)y.size(); n++) {
        const double ang = 6.283185307179586 * c.cfoCycPerSym * n / dect2::kavb::kSps + c.phase;
        y[n] *= cf((float)std::cos(ang), (float)std::sin(ang));
        if (c.snrDb < 99.0) y[n] += cf((float)(sigma * rng.gauss()), (float)(sigma * rng.gauss()));
    }
    return y;
}
}  // namespace kavbtest
