// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// std::normal_distribution and std::uniform_real_distribution that give the same numbers on every platform.
//
// The standard fixes the engines (std::mt19937 is the same everywhere) but not the distributions: libc++ (macOS) and libstdc++ (Linux, MinGW) turn the
// same engine output into different normal numbers (libstdc++ returns the second value of a pair first, and both differ in rounding details). The
// test signal generators and the tests were tuned with libc++, so on the other platforms the same seed gave another noise realisation, and the
// marginal ones (a lock at -10 dB echo, a beacon at 9 dB) failed there. These classes follow libc++'s algorithm step by step (float / double
// arithmetic included), so macOS keeps its numbers and Linux and Windows now get the same ones. Only the draws are shared, math functions are the
// platform's (logf / log differ in the last bit between libms, which does not matter for noise).
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace dect2 {

// std::hash<std::string> as libc++ computes it on a 64-bit target (CityHash64 for up to 64 bytes, the same constants and steps). libstdc++ (Linux,
// MinGW) hashes strings with another function, so a test that seeds its random numbers from std::hash of a name draws other numbers there than
// on macOS (test_specx_idmatrix did, and failed on Linux and Windows with figures tuned on macOS). This one gives libc++'s value everywhere.
// Strings longer than 64 bytes get a 64-bit FNV-1a (not libc++'s value; no caller hashes names that long).
inline uint64_t stable_string_hash(const char* s, size_t len) {
    const uint64_t k0 = 0xc3a5c85c97cb3127ULL, k1 = 0xb492b66fbe98f273ULL, k2 = 0x9ae16a3b2f90404fULL, k3 = 0xc949d7c7509e6557ULL;
    auto ld64 = [](const char* p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | (unsigned char)p[i]; return v; };
    auto ld32 = [](const char* p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | (unsigned char)p[i]; return v; };
    auto rot = [](uint64_t v, int sh) { return sh == 0 ? v : (v >> sh) | (v << (64 - sh)); };
    auto mix = [](uint64_t v) { return v ^ (v >> 47); };
    auto h16 = [](uint64_t u, uint64_t v) {
        const uint64_t mul = 0x9ddfea08eb382d69ULL;
        uint64_t a = (u ^ v) * mul; a ^= a >> 47;
        uint64_t b = (v ^ a) * mul; b ^= b >> 47;
        return b * mul;
    };
    const uint64_t n = len;
    if (len <= 16) {
        if (len > 8) {
            const uint64_t a = ld64(s), b = ld64(s + len - 8);
            const int sh = (int)(len & 63);
            return h16(a, ((b + n) >> sh) | ((b + n) << (64 - sh))) ^ b;
        }
        if (len >= 4) return h16(n + (uint32_t)(ld32(s) << 3), ld32(s + len - 4));   // libc++ shifts in 32 bits (its default ABI)
        if (len > 0) {
            const uint32_t y = (uint32_t)(unsigned char)s[0] + ((uint32_t)(unsigned char)s[len >> 1] << 8);
            const uint32_t z = (uint32_t)len + ((uint32_t)(unsigned char)s[len - 1] << 2);
            return mix(y * k2 ^ z * k3) * k2;
        }
        return k2;
    }
    if (len <= 32) {
        const uint64_t a = ld64(s) * k1, b = ld64(s + 8), c = ld64(s + len - 8) * k2, d = ld64(s + len - 16) * k0;
        return h16(rot(a - b, 43) + rot(c, 30) + d, a + rot(b ^ k3, 20) - c + n);
    }
    if (len <= 64) {
        uint64_t z = ld64(s + 24);
        uint64_t a = ld64(s) + (n + ld64(s + len - 16)) * k0;
        uint64_t b = rot(a + z, 52), c = rot(a, 37);
        a += ld64(s + 8); c += rot(a, 7); a += ld64(s + 16);
        const uint64_t vf = a + z, vs = b + rot(a, 31) + c;
        a = ld64(s + 16) + ld64(s + len - 32);
        z += ld64(s + len - 8);
        b = rot(a + z, 52); c = rot(a, 37);
        a += ld64(s + len - 24); c += rot(a, 7); a += ld64(s + len - 16);
        const uint64_t wf = a + z, ws = b + rot(a, 31) + c;
        const uint64_t r = mix((vf + ws) * k2 + (wf + vs) * k0);
        return mix(r * k0 + vs) * k2;
    }
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

namespace stable_random_detail {
// libc++ generate_canonical<Real, digits>(engine): `k` draws of the engine, scaled to [0, 1)
template <class Real, class Engine> Real canonical(Engine& g) {
    const int bits = std::numeric_limits<Real>::digits;
    // log2 of the engine's range (max - min + 1); a 64-bit engine has 2^64 values
    const uint64_t span = (uint64_t)(Engine::max() - Engine::min());
    int logR = 0;
    if (span == ~uint64_t(0)) logR = 64;
    else { uint64_t v = span + 1; while (v > 1) { v >>= 1; logR++; } }
    const int b = bits;
    const int k = b / logR + (b % logR != 0) + (b == 0);
    const Real rp = (Real)(std::ldexp(1.0L, logR));
    Real base = rp;
    Real sp = (Real)(g() - Engine::min());
    for (int i = 1; i < k; i++, base *= rp) sp += (Real)(g() - Engine::min()) * base;
    Real s = sp / base;
    if (s >= 1) s = std::nextafter((Real)1, (Real)0);
    return s;
}
}  // namespace stable_random_detail

template <class Real = double> class uniform_real_distribution {
public:
    using result_type = Real;
    explicit uniform_real_distribution(Real a = 0, Real b = 1) : a_(a), b_(b) {}
    template <class Engine> Real operator()(Engine& g) { return (b_ - a_) * stable_random_detail::canonical<Real>(g) + a_; }
private:
    Real a_, b_;
};

template <class Real = double> class normal_distribution {
public:
    using result_type = Real;
    explicit normal_distribution(Real mean = 0, Real stddev = 1) : mean_(mean), stddev_(stddev) {}
    template <class Engine> Real operator()(Engine& g) {
        Real r;
        if (hot_) {
            hot_ = false;
            r = saved_;
        } else {
            uniform_real_distribution<Real> uni(-1, 1);
            Real u, v, s;
            do {
                u = uni(g);
                v = uni(g);
                s = u * u + v * v;
            } while (s > 1 || s == 0);
            const Real fp = std::sqrt(-2 * std::log(s) / s);
            saved_ = v * fp;
            hot_ = true;
            r = u * fp;
        }
        return r * stddev_ + mean_;
    }
private:
    Real mean_, stddev_, saved_ = 0;
    bool hot_ = false;
};
}  // namespace dect2
