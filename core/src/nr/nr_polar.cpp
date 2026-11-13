// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 5G NR cells: the polar code of the downlink control channels (38.212 5.3.1 code construction and encoding, 5.4.1 rate matching) and a
// CRC-aided successive-cancellation list decoder.
#include "dect2/nr/nr_phy.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace dect2::nr {

#include "nr_polarq.inc"

namespace {

// 38.212 Table 5.3.1.1-1: the input interleaver pattern, K_IL_max = 164
const int kPiIlMax[164] = {
    0,   2,   4,   7,   9,   14,  19,  20,  24,  25,  26,  28,  31,  34,  42,  45,  49,  50,  51,  53,  54,  56,  58,  59,  61,  62,  65,  66,
    67,  69,  70,  71,  72,  76,  77,  81,  82,  83,  87,  88,  89,  91,  93,  95,  98,  101, 104, 106, 108, 110, 111, 113, 115, 118, 119, 120,
    122, 123, 126, 127, 129, 132, 134, 138, 139, 140, 1,   3,   5,   8,   10,  15,  21,  27,  29,  32,  35,  43,  46,  52,  55,  57,  60,  63,
    68,  73,  78,  84,  90,  92,  94,  96,  99,  102, 105, 107, 109, 112, 114, 116, 121, 124, 128, 130, 133, 135, 141, 6,   11,  16,  22,  30,
    33,  36,  44,  47,  64,  74,  79,  85,  97,  100, 103, 117, 125, 131, 136, 142, 12,  17,  23,  37,  48,  75,  80,  86,  137, 143, 13,  18,
    38,  144, 39,  145, 40,  146, 41,  147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163};

// 38.212 Table 5.4.1.1-1: the sub-block interleaver pattern
const int kSubblockP[32] = {0, 1, 2, 4, 3, 5, 6, 7, 8, 16, 9, 17, 10, 18, 11, 19, 12, 20, 13, 21, 14, 22, 15, 23, 24, 25, 26, 28, 27, 29, 30, 31};

int ceilLog2(double v) { int n = 0; while ((double)(1 << n) < v) n++; return n; }

} // namespace

PolarCode polarCode(int K, int E, int nMax, bool iil) {
    PolarCode pc;
    pc.K = K; pc.E = E; pc.iil = iil;
    // 5.3.1: the mother code size
    const int le = ceilLog2(E);
    int n1 = le;
    if ((double)E <= 9.0 / 8.0 * (double)(1 << (le - 1)) && (double)K / E < 9.0 / 16.0) n1 = le - 1;
    const int n2 = ceilLog2((double)K * 8.0);
    pc.n = std::max(std::min(std::min(n1, n2), nMax), 5);
    pc.N = 1 << pc.n;
    const int N = pc.N;
    pc.J.resize((size_t)N);
    for (int i = 0; i < N; i++) { const int s = 32 * i / N; pc.J[(size_t)i] = kSubblockP[s] * (N / 32) + i % (N / 32); }
    // 5.3.1.2: the frozen set
    std::vector<uint8_t> ftmp((size_t)N, 0);
    if (E < N) {
        if ((double)K / E <= 7.0 / 16.0) {   // puncturing
            for (int i = 0; i < N - E; i++) ftmp[(size_t)pc.J[(size_t)i]] = 1;
            const int t = E >= 3 * N / 4 ? (int)std::ceil(3.0 * N / 4 - E / 2.0) : (int)std::ceil(9.0 * N / 16 - E / 4.0);
            for (int i = 0; i < t && i < N; i++) ftmp[(size_t)i] = 1;
        } else {                              // shortening
            for (int i = E; i < N; i++) ftmp[(size_t)pc.J[(size_t)i]] = 1;
        }
    }
    pc.frozen.assign((size_t)N, 1);
    int need = K;
    for (int i = 511; i >= 0 && need > 0; i--) {   // the most reliable first
        const int q = kPolarQ512[i];
        if (q >= N || ftmp[(size_t)q]) continue;
        pc.frozen[(size_t)q] = 0;
        need--;
    }
    for (int i = 0; i < N; i++) if (!pc.frozen[(size_t)i]) pc.info.push_back(i);
    // 5.3.1.1: the input interleaver
    pc.pil.resize((size_t)K);
    if (iil) {
        int k = 0;
        for (int m = 0; m < 164 && k < K; m++) if (kPiIlMax[m] >= 164 - K) pc.pil[(size_t)k++] = kPiIlMax[m] - (164 - K);
    } else std::iota(pc.pil.begin(), pc.pil.end(), 0);
    return pc;
}

static void polarTransform(uint8_t* u, int N) {   // d = u G_N, G_N = F^(x)n, F = [1 0; 1 1]
    for (int h = 1; h < N; h <<= 1)
        for (int i = 0; i < N; i += 2 * h)
            for (int j = i; j < i + h; j++) u[j] ^= u[j + h];
}

Bits polarEncode(const PolarCode& pc, const Bits& c) {
    Bits u((size_t)pc.N, 0);
    for (int k = 0; k < pc.K; k++) u[(size_t)pc.info[(size_t)k]] = c[(size_t)pc.pil[(size_t)k]];
    polarTransform(u.data(), pc.N);
    return u;
}

Bits polarRateMatch(const PolarCode& pc, const Bits& d) {
    const int N = pc.N, E = pc.E;
    Bits y((size_t)N), e((size_t)E);
    for (int i = 0; i < N; i++) y[(size_t)i] = d[(size_t)pc.J[(size_t)i]];
    if (E >= N) for (int k = 0; k < E; k++) e[(size_t)k] = y[(size_t)(k % N)];
    else if ((double)pc.K / E <= 7.0 / 16.0) for (int k = 0; k < E; k++) e[(size_t)k] = y[(size_t)(k + N - E)];
    else for (int k = 0; k < E; k++) e[(size_t)k] = y[(size_t)k];
    return e;
}

std::vector<float> polarRateRecover(const PolarCode& pc, const float* e) {
    const int N = pc.N, E = pc.E;
    std::vector<float> y((size_t)N, 0.f), d((size_t)N, 0.f);
    if (E >= N) for (int k = 0; k < E; k++) y[(size_t)(k % N)] += e[k];
    else if ((double)pc.K / E <= 7.0 / 16.0) for (int k = 0; k < E; k++) y[(size_t)(k + N - E)] = e[k];   // punctured: 0 (unknown)
    else {
        for (int k = 0; k < E; k++) y[(size_t)k] = e[k];
        for (int k = E; k < N; k++) y[(size_t)k] = 1e4f;   // shortened: known zeros
    }
    for (int i = 0; i < N; i++) d[(size_t)pc.J[(size_t)i]] = y[(size_t)i];
    return d;
}

// ---------------------------------------------------------------------------------------------------------------- list decoder
namespace {

struct Path {
    std::vector<float> a;     // LLRs per level: level s (size 2^s) at offset 2^s - 1 ... the channel (level n) at offset N - 1
    std::vector<uint8_t> bl;  // left partial sums per level (same layout)
    std::vector<uint8_t> u;   // decided bits
    float pm = 0;
};

inline float fF(float a, float b) { const float m = std::min(std::fabs(a), std::fabs(b)); return ((a < 0) != (b < 0)) ? -m : m; }

} // namespace

bool polarDecode(const PolarCode& pc, const std::vector<float>& llr, int L, Bits& out, bool (*check)(const Bits&, void*), void* ctx) {
    const int N = pc.N, n = pc.n;
    std::vector<Path> paths(1);
    paths[0].a.assign((size_t)(2 * N), 0.f);
    paths[0].bl.assign((size_t)(2 * N), 0);
    paths[0].u.reserve((size_t)N);
    for (int i = 0; i < N; i++) paths[0].a[(size_t)(N - 1 + i)] = llr[(size_t)i];
    auto off = [](int s) { return (1 << s) - 1; };
    for (int i = 0; i < N; i++) {
        const int top = i == 0 ? n : (int)std::floor(std::log2((double)(i ^ (i - 1)))) + 1;   // levels to recompute: top .. 1
        for (auto& p : paths) {
            for (int s = top; s >= 1; s--) {
                const int h = 1 << (s - 1);
                const float* A = &p.a[(size_t)off(s)];
                float* B = &p.a[(size_t)off(s - 1)];
                if (((i >> (s - 1)) & 1) == 0) for (int j = 0; j < h; j++) B[j] = fF(A[j], A[j + h]);
                else {
                    const uint8_t* U = &p.bl[(size_t)off(s - 1)];
                    for (int j = 0; j < h; j++) B[j] = U[j] ? A[j + h] - A[j] : A[j + h] + A[j];
                }
            }
        }
        // decide leaf i
        auto commit = [&](Path& p, uint8_t bit) {
            p.u.push_back(bit);
            // propagate the partial sums upwards
            std::vector<uint8_t> v(1, bit), w;
            for (int s = 1; s <= n; s++) {
                const int h = 1 << (s - 1);
                if (((i >> (s - 1)) & 1) == 0) { std::copy(v.begin(), v.end(), p.bl.begin() + off(s - 1)); break; }
                w.resize((size_t)(2 * h));
                const uint8_t* U = &p.bl[(size_t)off(s - 1)];
                for (int j = 0; j < h; j++) { w[(size_t)j] = U[j] ^ v[(size_t)j]; w[(size_t)(j + h)] = v[(size_t)j]; }
                v.swap(w);
            }
        };
        if (pc.frozen[(size_t)i]) {
            for (auto& p : paths) { const float l = p.a[0]; if (l < 0) p.pm += -l; commit(p, 0); }
            continue;
        }
        std::vector<std::pair<float, std::pair<int, uint8_t>>> c;
        c.reserve(paths.size() * 2);
        for (size_t k = 0; k < paths.size(); k++) {
            const float l = paths[k].a[0];
            c.push_back({paths[k].pm + (l < 0 ? -l : 0.f), {(int)k, (uint8_t)0}});
            c.push_back({paths[k].pm + (l > 0 ? l : 0.f), {(int)k, (uint8_t)1}});
        }
        std::sort(c.begin(), c.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        if ((int)c.size() > L) c.resize((size_t)L);
        std::vector<Path> next;
        next.reserve(c.size());
        for (const auto& e : c) { next.push_back(paths[(size_t)e.second.first]); next.back().pm = e.first; commit(next.back(), e.second.second); }
        paths.swap(next);
    }
    std::sort(paths.begin(), paths.end(), [](const Path& x, const Path& y) { return x.pm < y.pm; });
    for (const auto& p : paths) {
        Bits cc((size_t)pc.K);
        for (int k = 0; k < pc.K; k++) cc[(size_t)pc.pil[(size_t)k]] = p.u[(size_t)pc.info[(size_t)k]];
        if (!check || check(cc, ctx)) { out = cc; return true; }
    }
    return false;
}

} // namespace dect2::nr
