// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers: channel coding of the LTE broadcast channels (3GPP TS 36.212), see cell_fec.h.
#include "dect2/cell/cell_fec.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::cell {

const int kSubblockPerm[32] = {0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30, 1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31};
const int kSubblockPermConv[32] = {1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31, 0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30};

uint32_t crc(const uint8_t* a, size_t n, uint32_t poly, int len) {
    const uint32_t top = 1u << (len - 1), mask = (len == 32) ? 0xffffffffu : ((1u << len) - 1);
    uint32_t r = 0;
    for (size_t i = 0; i < n; i++) {
        const bool fb = ((r & top) != 0) ^ (a[i] & 1);
        r = (r << 1) & mask;
        if (fb) r ^= poly;
    }
    return r & mask;
}

void appendCrc(Bits& b, uint32_t poly, int len, uint32_t xorMask) {
    const uint32_t p = crc(b.data(), b.size(), poly, len) ^ xorMask;
    for (int i = len - 1; i >= 0; i--) b.push_back((p >> i) & 1);
}

bool checkCrc(const Bits& b, uint32_t poly, int len, uint32_t xorMask) {
    if ((int)b.size() < len) return false;
    const size_t n = b.size() - (size_t)len;
    uint32_t p = 0;
    for (int i = 0; i < len; i++) p = (p << 1) | b[n + (size_t)i];
    return (crc(b.data(), n, poly, len) ^ xorMask) == p;
}

void gold(uint32_t cinit, uint8_t* c, size_t len) {
    const size_t Nc = 1600, n = len + Nc + 31;
    std::vector<uint8_t> x1(n, 0), x2(n, 0);
    x1[0] = 1;
    for (int i = 0; i < 31; i++) x2[(size_t)i] = (cinit >> i) & 1;
    for (size_t i = 0; i + 31 < n; i++) {
        x1[i + 31] = x1[i + 3] ^ x1[i];
        x2[i + 31] = x2[i + 3] ^ x2[i + 2] ^ x2[i + 1] ^ x2[i];
    }
    for (size_t i = 0; i < len; i++) c[i] = x1[i + Nc] ^ x2[i + Nc];
}

Bits gold(uint32_t cinit, size_t len) { Bits c(len); gold(cinit, c.data(), len); return c; }

// ---------------------------------------------------------------- convolutional code
static const int kG[3] = {0133, 0171, 0165};
static inline int par(unsigned v) { return __builtin_popcount(v) & 1; }

Bits convEncode(const Bits& in) {
    const size_t K = in.size();
    Bits out(3 * K);
    unsigned reg = 0;
    for (int j = 0; j < 6 && (size_t)j < K; j++) reg |= (unsigned)(in[K - 1 - (size_t)j] & 1) << (5 - j);
    for (size_t k = 0; k < K; k++) {
        const unsigned w = ((unsigned)(in[k] & 1) << 6) | reg;
        for (int i = 0; i < 3; i++) out[(size_t)i * K + k] = (uint8_t)par(w & (unsigned)kG[i]);
        reg = w >> 1;
    }
    return out;
}

// index map of the 32-column sub-block interleaver: v index -> d index (-1 = NULL); stream 2 of the turbo code uses its own permutation
static std::vector<int> subblockMap(int D, bool turboStream2, const int* perm = kSubblockPerm) {
    const int R = (D + 31) / 32, Kpi = 32 * R, Nd = Kpi - D;
    std::vector<int> m((size_t)Kpi);
    for (int k = 0; k < Kpi; k++) {
        int y;
        if (!turboStream2) y = (k % R) * 32 + perm[k / R];
        else y = (kSubblockPerm[k / R] + 32 * (k % R) + 1) % Kpi;
        m[(size_t)k] = y - Nd;   // negative: a dummy bit
    }
    return m;
}

Bits convRateMatch(const Bits& d, size_t E) {
    const int K = (int)(d.size() / 3);
    const std::vector<int> m = subblockMap(K, false, kSubblockPermConv);
    std::vector<int> w;   // positions in d
    for (int s = 0; s < 3; s++) for (int v : m) w.push_back(v < 0 ? -1 : s * K + v);
    Bits e(E);
    size_t k = 0, j = 0;
    while (k < E) { const int p = w[j % w.size()]; if (p >= 0) e[k++] = d[(size_t)p]; j++; }
    return e;
}

std::vector<float> convRateRecover(const float* e, size_t E, size_t K) {
    const std::vector<int> m = subblockMap((int)K, false, kSubblockPermConv);
    std::vector<int> w;
    for (int s = 0; s < 3; s++) for (int v : m) w.push_back(v < 0 ? -1 : s * (int)K + v);
    std::vector<float> d(3 * K, 0.f);
    size_t k = 0, j = 0;
    while (k < E) { const int p = w[j % w.size()]; if (p >= 0) d[(size_t)p] += e[k++]; j++; }
    return d;
}

Bits viterbiTb(const float* llr, size_t K) {
    if (K == 0) return {};
    const size_t N = 3 * K;   // three passes round the tail-biting circle; the middle one is kept
    std::vector<float> pm(64, 0.f), nm(64);
    std::vector<uint8_t> dec(N * 64);
    int outBits[64][3];
    for (unsigned w = 0; w < 128; w++) { (void)w; }
    for (int ns = 0; ns < 64; ns++) for (int i = 0; i < 3; i++) outBits[ns][i] = 0;
    for (size_t t = 0; t < N; t++) {
        const size_t k = t % K;
        const float l0 = llr[k], l1 = llr[K + k], l2 = llr[2 * K + k];
        for (int ns = 0; ns < 64; ns++) {
            const unsigned u = (unsigned)ns >> 5;
            float best = -1e30f; int bb = 0;
            for (int b = 0; b < 2; b++) {
                const unsigned reg = (((unsigned)ns & 31u) << 1) | (unsigned)b;
                const unsigned w = (u << 6) | reg;
                const float m = pm[reg] + (par(w & 0133) ? -l0 : l0) + (par(w & 0171) ? -l1 : l1) + (par(w & 0165) ? -l2 : l2);
                if (m > best) { best = m; bb = b; }
            }
            nm[(size_t)ns] = best;
            dec[t * 64 + (size_t)ns] = (uint8_t)bb;
        }
        const float mx = *std::max_element(nm.begin(), nm.end());
        for (int s = 0; s < 64; s++) pm[(size_t)s] = nm[(size_t)s] - mx;
    }
    int s = (int)(std::max_element(pm.begin(), pm.end()) - pm.begin());
    Bits all(N);
    for (size_t t = N; t-- > 0;) {
        all[t] = (uint8_t)(s >> 5);
        s = ((s & 31) << 1) | dec[t * 64 + (size_t)s];
    }
    return Bits(all.begin() + (long)K, all.begin() + (long)(2 * K));
}

// ---------------------------------------------------------------- turbo code
static const int kQpp[188][3] = {
#include "cell_qpp.inc"
};

bool turboSizeOk(int K) { for (auto& q : kQpp) if (q[0] == K) return true; return false; }
int turboSizeFor(int B) { for (auto& q : kQpp) if (q[0] >= B) return q[0]; return 0; }
void qppParams(int K, int& f1, int& f2) {
    f1 = f2 = 0;
    for (auto& q : kQpp) if (q[0] == K) { f1 = q[1]; f2 = q[2]; return; }
}

static std::vector<int> qpp(int K) {
    int f1, f2; qppParams(K, f1, f2);
    std::vector<int> p((size_t)K);
    for (int i = 0; i < K; i++) p[(size_t)i] = (int)(((int64_t)f1 * i + (int64_t)f2 * i % K * i) % K);
    return p;
}

// one constituent encoder: z for each input, and the 3 tail (x, z) pairs
static void rscEncode(const uint8_t* c, int K, uint8_t* z, uint8_t tx[3], uint8_t tz[3]) {
    int s1 = 0, s2 = 0, s3 = 0;
    for (int k = 0; k < K; k++) {
        const int a = c[k] ^ s2 ^ s3;
        z[k] = (uint8_t)(a ^ s1 ^ s3);
        s3 = s2; s2 = s1; s1 = a;
    }
    for (int k = 0; k < 3; k++) {
        const int u = s2 ^ s3, a = 0;
        tx[k] = (uint8_t)u; tz[k] = (uint8_t)(a ^ s1 ^ s3);
        s3 = s2; s2 = s1; s1 = a;
    }
}

Bits turboEncode(const Bits& c) {
    const int K = (int)c.size();
    const std::vector<int> pi = qpp(K);
    Bits ci((size_t)K);
    for (int i = 0; i < K; i++) ci[(size_t)i] = c[(size_t)pi[(size_t)i]];
    std::vector<uint8_t> z((size_t)K), zi((size_t)K);
    uint8_t x1[3], z1[3], x2[3], z2[3];
    rscEncode(c.data(), K, z.data(), x1, z1);
    rscEncode(ci.data(), K, zi.data(), x2, z2);
    const int L = K + 4;
    Bits d((size_t)(3 * L));
    for (int k = 0; k < K; k++) { d[(size_t)k] = c[(size_t)k]; d[(size_t)(L + k)] = z[(size_t)k]; d[(size_t)(2 * L + k)] = zi[(size_t)k]; }
    uint8_t* d0 = &d[0]; uint8_t* d1 = &d[(size_t)L]; uint8_t* d2 = &d[(size_t)(2 * L)];
    d0[K] = x1[0]; d0[K + 1] = z1[1]; d0[K + 2] = x2[0]; d0[K + 3] = z2[1];
    d1[K] = z1[0]; d1[K + 1] = x1[2]; d1[K + 2] = z2[0]; d1[K + 3] = x2[2];
    d2[K] = x1[1]; d2[K + 1] = z1[2]; d2[K + 2] = x2[1]; d2[K + 3] = z2[2];
    return d;
}

// circular buffer of a turbo code block: positions in d012 (-1 = NULL), and k0
static std::vector<int> turboBuffer(int K, int F, int rv, int& k0) {
    const int D = K + 4, R = (D + 31) / 32, Kpi = 32 * R;
    const std::vector<int> m01 = subblockMap(D, false), m2 = subblockMap(D, true);
    std::vector<int> w((size_t)(3 * Kpi));
    for (int k = 0; k < Kpi; k++) {
        const int v = m01[(size_t)k];
        w[(size_t)k] = (v < 0 || v < F) ? -1 : v;
        w[(size_t)(Kpi + 2 * k)] = (v < 0 || v < F) ? -1 : D + v;
        const int v2 = m2[(size_t)k];
        w[(size_t)(Kpi + 2 * k + 1)] = v2 < 0 ? -1 : 2 * D + v2;
    }
    const int Ncb = 3 * Kpi;
    k0 = R * (2 * ((Ncb + 8 * R - 1) / (8 * R)) * rv + 2);
    return w;
}

Bits turboRateMatch(const Bits& d, int K, int F, size_t E, int rv) {
    int k0; const std::vector<int> w = turboBuffer(K, F, rv, k0);
    Bits e(E);
    size_t k = 0, j = 0;
    while (k < E) { const int p = w[(k0 + j) % w.size()]; if (p >= 0) e[k++] = d[(size_t)p]; j++; }
    return e;
}

void turboRateRecover(const float* e, size_t E, int K, int F, int rv, std::vector<float>& d) {
    int k0; const std::vector<int> w = turboBuffer(K, F, rv, k0);
    if (d.size() != (size_t)(3 * (K + 4))) d.assign((size_t)(3 * (K + 4)), 0.f);
    size_t k = 0, j = 0;
    while (k < E) { const int p = w[(k0 + j) % w.size()]; if (p >= 0) d[(size_t)p] += e[k++]; j++; }
}

// max-log-MAP of one constituent code. sys/par: K values; tx/tz: tail; la: a-priori (K); out: total LLR (K)
static void bcjr(const float* sys, const float* par, const float* la, const float tx[3], const float tz[3], int K, float* out) {
    const int N = K + 3;
    static thread_local std::vector<float> A, B;
    A.assign((size_t)(N + 1) * 8, -1e30f);
    B.assign((size_t)(N + 1) * 8, -1e30f);
    A[0] = 0;
    // transitions: from s with input u -> ns, parity z
    int nsT[8][2], zT[8][2];
    for (int s = 0; s < 8; s++) for (int u = 0; u < 2; u++) {
        const int s1 = (s >> 2) & 1, s2 = (s >> 1) & 1, s3 = s & 1;
        const int a = u ^ s2 ^ s3;
        zT[s][u] = a ^ s1 ^ s3;
        nsT[s][u] = (a << 2) | (s1 << 1) | s2;
    }
    auto gam = [&](int k, int s, int u, float& g) -> bool {
        if (k < K) { g = 0.5f * ((sys[k] + la[k]) * (u ? -1.f : 1.f) + par[k] * (zT[s][u] ? -1.f : 1.f)); return true; }
        const int s2 = (s >> 1) & 1, s3 = s & 1;
        if (u != (s2 ^ s3)) return false;
        g = 0.5f * (tx[k - K] * (u ? -1.f : 1.f) + tz[k - K] * (zT[s][u] ? -1.f : 1.f));
        return true;
    };
    for (int k = 0; k < N; k++) {
        float* a = &A[(size_t)k * 8]; float* an = &A[(size_t)(k + 1) * 8];
        for (int s = 0; s < 8; s++) {
            if (a[s] < -1e29f) continue;
            for (int u = 0; u < 2; u++) { float g; if (!gam(k, s, u, g)) continue; const int ns = nsT[s][u]; an[ns] = std::max(an[ns], a[s] + g); }
        }
        float mx = -1e30f; for (int s = 0; s < 8; s++) mx = std::max(mx, an[s]);
        for (int s = 0; s < 8; s++) an[s] -= mx;
    }
    B[(size_t)N * 8 + 0] = 0;
    for (int k = N - 1; k >= 0; k--) {
        float* b = &B[(size_t)k * 8]; const float* bn = &B[(size_t)(k + 1) * 8];
        for (int s = 0; s < 8; s++) for (int u = 0; u < 2; u++) {
            float g; if (!gam(k, s, u, g)) continue;
            const int ns = nsT[s][u];
            if (bn[ns] < -1e29f) continue;
            b[s] = std::max(b[s], bn[ns] + g);
        }
        float mx = -1e30f; for (int s = 0; s < 8; s++) mx = std::max(mx, b[s]);
        for (int s = 0; s < 8; s++) b[s] -= mx;
        if (k < K) {
            const float* a = &A[(size_t)k * 8];
            float m0 = -1e30f, m1 = -1e30f;
            for (int s = 0; s < 8; s++) {
                if (a[s] < -1e29f) continue;
                for (int u = 0; u < 2; u++) {
                    float g; gam(k, s, u, g);
                    const float v = a[s] + g + bn[nsT[s][u]];
                    if (u) m1 = std::max(m1, v); else m0 = std::max(m0, v);
                }
            }
            out[k] = m0 - m1;
        }
    }
}

bool turboDecode(const std::vector<float>& d, int K, int F, int iterations, Bits& out, bool (*check)(const Bits&, void*), void* ctx) {
    const int L = K + 4;
    const float* d0 = &d[0]; const float* d1 = &d[(size_t)L]; const float* d2 = &d[(size_t)(2 * L)];
    const std::vector<int> pi = qpp(K);
    std::vector<float> sys(d0, d0 + K), sysI((size_t)K), la((size_t)K, 0.f), le((size_t)K), laI((size_t)K), tot((size_t)K);
    for (int k = 0; k < F; k++) sys[(size_t)k] = 50.f;   // known zeros
    for (int i = 0; i < K; i++) sysI[(size_t)i] = sys[(size_t)pi[(size_t)i]];
    const float tx1[3] = {d0[K], d2[K], d1[K + 1]}, tz1[3] = {d1[K], d0[K + 1], d2[K + 1]};
    const float tx2[3] = {d0[K + 2], d2[K + 2], d1[K + 3]}, tz2[3] = {d1[K + 2], d0[K + 3], d2[K + 3]};
    out.assign((size_t)K, 0);
    const float sc = 0.75f;
    for (int it = 0; it < iterations; it++) {
        bcjr(sys.data(), d1, la.data(), tx1, tz1, K, tot.data());
        for (int k = 0; k < K; k++) le[(size_t)k] = sc * (tot[(size_t)k] - sys[(size_t)k] - la[(size_t)k]);
        for (int i = 0; i < K; i++) laI[(size_t)i] = le[(size_t)pi[(size_t)i]];
        bcjr(sysI.data(), d2, laI.data(), tx2, tz2, K, tot.data());
        for (int i = 0; i < K; i++) {
            const int k = pi[(size_t)i];
            la[(size_t)k] = sc * (tot[(size_t)i] - sysI[(size_t)i] - laI[(size_t)i]);
            out[(size_t)k] = tot[(size_t)i] < 0;
        }
        if (check && check(out, ctx)) return true;
    }
    return check ? false : true;
}

} // namespace dect2::cell
