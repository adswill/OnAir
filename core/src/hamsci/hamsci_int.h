// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Internal helpers of the ham science modes: a small FFT, a noise source, a Fano decoder for the K = 32 rate 1/2 code.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace hamsci {
namespace detail {

constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;

inline void fft(std::vector<cd>& a) {   // in place, size a power of two
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / (double)len;
        const cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cd w(1, 0);
            for (size_t k = 0; k < len / 2; k++) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}
inline size_t nextPow2(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

struct Rng {   // xorshift + Box-Muller
    uint64_t s;
    explicit Rng(uint32_t seed = 1) : s(0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0xD1B54A32D192ED03ull)) { for (int i = 0; i < 4; i++) next(); }
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    double uni() { return ((next() >> 11) + 0.5) / 9007199254740992.0; }
    double gauss() { const double u = uni(), v = uni(); return std::sqrt(-2 * std::log(u)) * std::cos(2 * kPi * v); }
};

inline int parity32(uint32_t x) { x ^= x >> 16; x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1; return (int)(x & 1); }

// Fano sequential decoder for the WSJT K = 32, rate 1/2 code (polynomials 0xF2D05351, 0xE4613C47): nBits input bits of which the first nData are free,
// the rest zero tail. llr: 2 * nBits values, positive = the coded bit is 1. Returns false when it gives up after maxCycles. Written after the descriptions of
// Karn's fano.c as used in WSJT; compare core/src/hfdig/hfdig_wspr.cpp (the same algorithm for 81 bits).
inline bool fanoDecode(const float* llr, int nBits, int nData, uint8_t* bitsOut, long maxCycles) {
    constexpr uint32_t kP1 = 0xF2D05351u, kP2 = 0xE4613C47u;
    auto met = [](float l, int bit) {
        const float x = bit ? l : -l;
        const float p = 1.f / (1.f + std::exp(-std::max(-20.f, std::min(20.f, x))));
        return (int)std::lround(8.0 * (std::log2(2.0 * std::max(1e-6f, p)) - 0.5));
    };
    std::vector<int> m((size_t)nBits * 4);
    for (int i = 0; i < nBits; i++) for (int b = 0; b < 2; b++) { m[(size_t)(2 * i + b) * 2] = met(llr[2 * i + b], 0); m[(size_t)(2 * i + b) * 2 + 1] = met(llr[2 * i + b], 1); }
    struct Node { uint32_t enc; int gamma; int tm[2]; int i; };
    std::vector<Node> nodes((size_t)nBits + 1);
    auto branch = [&](int k, uint32_t state) { return m[(size_t)(2 * k) * 2 + (size_t)parity32(state & kP1)] + m[(size_t)(2 * k + 1) * 2 + (size_t)parity32(state & kP2)]; };
    auto fill = [&](int k) {
        Node& n = nodes[(size_t)k];
        const uint32_t s0 = n.enc;
        const int m0 = branch(k, s0);
        if (k >= nData) { n.tm[0] = m0; n.tm[1] = 0; return; }
        const int m1 = branch(k, s0 | 1);
        if (m0 >= m1) { n.tm[0] = m0; n.tm[1] = m1; }
        else { n.tm[0] = m1; n.tm[1] = m0; n.enc |= 1; }
    };
    const int delta = 16;
    int t = 0, np = 0;
    nodes[0].enc = 0; nodes[0].gamma = 0; nodes[0].i = 0;
    fill(0);
    bool done = false;
    for (long cyc = 0; cyc < maxCycles; cyc++) {
        Node& n = nodes[(size_t)np];
        const int ng = n.gamma + n.tm[n.i];
        if (ng >= t) {
            if (n.gamma < t + delta) while (ng >= t + delta) t += delta;
            Node& nx = nodes[(size_t)np + 1];
            nx.gamma = ng;
            nx.enc = n.enc << 1;
            if (++np == nBits) { done = true; break; }
            fill(np);
            nodes[(size_t)np].i = 0;
            continue;
        }
        for (;;) {
            if (np == 0 || nodes[(size_t)np - 1].gamma < t) {
                t -= delta;
                if (nodes[(size_t)np].i != 0) { nodes[(size_t)np].i = 0; nodes[(size_t)np].enc ^= 1; }
                break;
            }
            --np;
            if (np < nData && nodes[(size_t)np].i != 1) { nodes[(size_t)np].i++; nodes[(size_t)np].enc ^= 1; break; }
        }
    }
    if (!done) return false;
    for (int k = 0; k < nData; k++) bitsOut[k] = (uint8_t)(nodes[(size_t)k].enc & 1);
    return true;
}

}  // namespace detail
}  // namespace hamsci
}  // namespace dect2
