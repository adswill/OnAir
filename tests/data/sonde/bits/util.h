// Shared helpers of the test_sonde_bits / _dfm / _m10 / _m20 programs (a header, kept here so that it is delivered with the tests).
#pragma once
#include "dect2/sonde_bits.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace sbt {

using namespace dect2;

inline int& fails() { static int f = 0; return f; }
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); sbt::fails()++; } } while (0)

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 2685821657736338717ull + 88172645463325252ull) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 16); }
    double uni() { return (next() & 0xFFFFFF) / 16777216.0; }
    int range(int lo, int hi) { return lo + (int)(next() % (uint32_t)(hi - lo + 1)); }
};

inline std::vector<uint8_t> randomSymbols(Rng& r, size_t n) {
    std::vector<uint8_t> v(n);
    for (auto& x : v) x = (uint8_t)(r.next() & 1);
    return v;
}

// feed in chunks, collect everything
inline std::vector<SondeFix> feed(SondeBitDecoder& d, const std::vector<uint8_t>& s, size_t chunk, double t0 = 0, double rate = 0) {
    std::vector<SondeFix> out;
    const double sr = rate > 0 ? rate : d.symbolRate();
    for (size_t i = 0; i < s.size(); i += chunk) {
        const size_t n = std::min(chunk, s.size() - i);
        d.push(s.data() + i, n, t0 + (double)i / sr, out);
    }
    return out;
}

inline void invert(std::vector<uint8_t>& s) { for (auto& x : s) x ^= 1; }

inline int countOk(const std::vector<SondeFix>& v) { int n = 0; for (const auto& f : v) n += f.crcOk; return n; }

// Flip every symbol with probability p
inline int flipSymbols(Rng& r, std::vector<uint8_t>& s, double p) {
    int n = 0;
    for (auto& x : s) if (r.uni() < p) { x ^= 1; n++; }
    return n;
}

} // namespace sbt
