// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Portable random numbers for test signals: the std distributions (normal, uniform) are implemented differently by libstdc++,
// libc++ and MSVC, so the same seed gives different noise on each platform. This generator gives the same stream everywhere.
#pragma once
#include <cmath>
#include <cstdint>

namespace dect2 {

struct PortableRng {
    uint64_t s;
    explicit PortableRng(uint64_t seed = 1) : s(mix(seed ^ 0x632BE59BD9B4E019ull)) {}   // mixed: nearby seeds give unrelated streams
    static uint64_t mix(uint64_t z) {
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint64_t next() {   // splitmix64
        return mix(s += 0x9E3779B97F4A7C15ull);
    }
    void seed(uint64_t v) { *this = PortableRng(v); }
    uint32_t operator()() { return (uint32_t)(next() >> 32); }
    double uniform() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }   // [0, 1)
    double uniform(double a, double b) { return a + (b - a) * uniform(); }
    uint64_t below(uint64_t n) { return n ? next() % n : 0; }
    double gauss() {   // Box-Muller, the second value is kept for the next call
        if (haveSpare) { haveSpare = false; return spare; }
        double u = uniform();
        while (u <= 0) u = uniform();
        const double r = std::sqrt(-2.0 * std::log(u)), a = 6.283185307179586 * uniform();
        spare = r * std::sin(a); haveSpare = true;
        return r * std::cos(a);
    }
    double gauss(double sigma) { return sigma * gauss(); }
    double spare = 0;
    bool haveSpare = false;
};

}  // namespace dect2
