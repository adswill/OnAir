// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/kaal/kaal_fec.h"
#include <cmath>
#include <cstring>

namespace dect2 {
namespace kaal {

// Base matrix of shifts (-1 = zero block), from the Python prototype (seed 0, column weights 6,3,3,..), 4-cycle free.
static const int kBase[kLdpcMb][kLdpcNb] = {
    {17,-1,28,-1,-1,-1,-1,-1,24,-1,1,0,-1,-1,-1,-1,-1,-1,-1,-1},
    {2,-1,-1,-1,30,-1,10,-1,-1,30,-1,0,0,-1,-1,-1,-1,-1,-1,-1},
    {0,-1,-1,-1,-1,29,-1,-1,9,26,-1,-1,0,0,-1,-1,-1,-1,-1,-1},
    {-1,12,-1,-1,15,-1,-1,28,-1,-1,-1,-1,-1,0,0,-1,-1,-1,-1,-1},
    {-1,22,-1,24,-1,5,-1,-1,-1,-1,-1,-1,-1,-1,0,0,-1,-1,-1,-1},
    {29,-1,-1,-1,6,-1,7,-1,-1,-1,0,-1,-1,-1,-1,0,0,-1,-1,-1},
    {-1,-1,10,1,-1,-1,-1,16,-1,4,-1,-1,-1,-1,-1,-1,0,0,-1,-1},
    {2,-1,-1,-1,-1,29,-1,-1,25,-1,-1,-1,-1,-1,-1,-1,-1,0,0,-1},
    {-1,28,8,-1,-1,-1,-1,4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,0,0},
    {27,-1,-1,18,-1,-1,28,-1,-1,-1,1,-1,-1,-1,-1,-1,-1,-1,-1,0}
};

namespace {
constexpr int kMaxDc = 16;

struct Code {
    int dc[kLdpcM];                 // row weights
    int var[kLdpcM][kMaxDc];        // variable indices per check, ascending
    uint64_t pm[kLdpcM][10];        // parity-generator rows: parity bit r = parity(pm[r] & info), 320 info bits in words 0..4
    Code() {
        // expand H
        static uint8_t H[kLdpcM][kLdpcN];
        std::memset(H, 0, sizeof(H));
        for (int r = 0; r < kLdpcMb; r++)
            for (int c = 0; c < kLdpcNb; c++)
                if (kBase[r][c] >= 0)
                    for (int i = 0; i < kLdpcZ; i++) H[r * kLdpcZ + i][c * kLdpcZ + (i + kBase[r][c]) % kLdpcZ] = 1;
        for (int i = 0; i < kLdpcM; i++) {
            int k = 0;
            for (int j = 0; j < kLdpcN; j++) if (H[i][j]) var[i][k++] = j;
            dc[i] = k;
        }
        // Pm = Hp^-1 * Hi over GF(2): row-reduce [Hp | Hi] packed as 640 bits (Hp in bits 0..319, Hi in bits 320..639)
        static uint64_t A[kLdpcM][10];
        std::memset(A, 0, sizeof(A));
        for (int i = 0; i < kLdpcM; i++) {
            for (int j = 0; j < kLdpcM; j++) if (H[i][kLdpcK + j]) A[i][j >> 6] |= 1ULL << (j & 63);
            for (int j = 0; j < kLdpcK; j++) if (H[i][j]) { const int b = kLdpcM + j; A[i][b >> 6] |= 1ULL << (b & 63); }
        }
        for (int c = 0; c < kLdpcM; c++) {
            int p = c;
            while (p < kLdpcM && !((A[p][c >> 6] >> (c & 63)) & 1)) p++;
            if (p == kLdpcM) continue;   // cannot happen for this code (dual-diagonal parity part is invertible)
            if (p != c) for (int w = 0; w < 10; w++) { const uint64_t t = A[p][w]; A[p][w] = A[c][w]; A[c][w] = t; }
            for (int r = 0; r < kLdpcM; r++)
                if (r != c && ((A[r][c >> 6] >> (c & 63)) & 1))
                    for (int w = 0; w < 10; w++) A[r][w] ^= A[c][w];
        }
        // right half (bits 320..639) -> pm rows with info bit j at bit j
        for (int r = 0; r < kLdpcM; r++) {
            std::memset(pm[r], 0, sizeof(pm[r]));
            for (int j = 0; j < kLdpcK; j++) {
                const int b = kLdpcM + j;
                if ((A[r][b >> 6] >> (b & 63)) & 1) pm[r][j >> 6] |= 1ULL << (j & 63);
            }
        }
    }
};

const Code& code() { static const Code c; return c; }
}  // namespace

void ldpcEncode(const uint8_t* info, uint8_t* out) {
    const Code& c = code();
    uint64_t u[5] = {0, 0, 0, 0, 0};
    for (int j = 0; j < kLdpcK; j++) { out[j] = info[j] & 1; if (out[j]) u[j >> 6] |= 1ULL << (j & 63); }
    for (int r = 0; r < kLdpcM; r++) {
        uint64_t x = 0;
        for (int w = 0; w < 5; w++) x ^= c.pm[r][w] & u[w];
        x ^= x >> 32; x ^= x >> 16; x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
        out[kLdpcK + r] = (uint8_t)(x & 1);
    }
}

bool ldpcDecode(const float* llr, uint8_t* info, int maxIter, float alpha, int* iterationsUsed) {
    const Code& c = code();
    static thread_local float R[kLdpcM][kMaxDc];
    float post[kLdpcN];
    std::memset(R, 0, sizeof(R));
    const float big = 1e9f;
    bool ok = false;
    int it = 0;
    for (; it < maxIter; it++) {
        for (int v = 0; v < kLdpcN; v++) post[v] = llr[v];
        for (int m = 0; m < kLdpcM; m++) for (int k = 0; k < c.dc[m]; k++) post[c.var[m][k]] += R[m][k];
        ok = true;
        for (int m = 0; m < kLdpcM && ok; m++) {
            int s = 0;
            for (int k = 0; k < c.dc[m]; k++) s ^= post[c.var[m][k]] < 0;
            if (s) ok = false;
        }
        if (ok) break;
        for (int m = 0; m < kLdpcM; m++) {
            float q[kMaxDc], m1 = big, m2 = big;
            int i1 = 0;
            float tsign = 1.f;
            for (int k = 0; k < c.dc[m]; k++) {
                q[k] = post[c.var[m][k]] - R[m][k];
                const float a = std::fabs(q[k]);
                if (q[k] < 0) tsign = -tsign;
                if (a < m1) { m2 = m1; m1 = a; i1 = k; } else if (a < m2) m2 = a;
            }
            for (int k = 0; k < c.dc[m]; k++) {
                const float sg = q[k] < 0 ? -1.f : 1.f;
                R[m][k] = alpha * tsign * sg * (k == i1 ? m2 : m1);
            }
        }
    }
    for (int v = 0; v < kLdpcN; v++) post[v] = llr[v];
    for (int m = 0; m < kLdpcM; m++) for (int k = 0; k < c.dc[m]; k++) post[c.var[m][k]] += R[m][k];
    ok = true;
    for (int m = 0; m < kLdpcM && ok; m++) {
        int s = 0;
        for (int k = 0; k < c.dc[m]; k++) s ^= post[c.var[m][k]] < 0;
        if (s) ok = false;
    }
    for (int j = 0; j < kLdpcK; j++) info[j] = post[j] < 0;
    if (iterationsUsed) *iterationsUsed = it;
    return ok;
}

bool ldpcDecodeLayered(const float* llr, uint8_t* info, int maxIter, float alpha, float* postOut, int* iterationsUsed) {
    const Code& c = code();
    static thread_local float R[kLdpcM][kMaxDc];
    float post[kLdpcN];
    std::memset(R, 0, sizeof(R));
    for (int v = 0; v < kLdpcN; v++) post[v] = llr[v];
    auto checksOk = [&]() {
        for (int m = 0; m < kLdpcM; m++) {
            int s = 0;
            for (int k = 0; k < c.dc[m]; k++) s ^= post[c.var[m][k]] < 0;
            if (s) return false;
        }
        return true;
    };
    bool ok = checksOk();
    int it = 0;
    for (; it < maxIter && !ok; it++) {
        for (int m = 0; m < kLdpcM; m++) {   // rows of one base row touch every variable at most once, so row order = layer order
            float q[kMaxDc], m1 = 1e30f, m2 = 1e30f, tsign = 1.f;
            int i1 = 0;
            const int dc = c.dc[m];
            for (int k = 0; k < dc; k++) {
                q[k] = post[c.var[m][k]] - R[m][k];
                const float a = std::fabs(q[k]);
                if (q[k] < 0) tsign = -tsign;
                if (a < m1) { m2 = m1; m1 = a; i1 = k; } else if (a < m2) m2 = a;
            }
            for (int k = 0; k < dc; k++) {
                const float sg = q[k] < 0 ? -1.f : 1.f;
                const float r = alpha * tsign * sg * (k == i1 ? m2 : m1);
                R[m][k] = r;
                post[c.var[m][k]] = q[k] + r;
            }
        }
        ok = checksOk();
    }
    for (int j = 0; j < kLdpcK; j++) info[j] = post[j] < 0;
    if (postOut) for (int v = 0; v < kLdpcN; v++) postOut[v] = post[v];
    if (iterationsUsed) *iterationsUsed = it;
    return ok;
}

void ldpcEncodeRm(const uint8_t* info, int k, uint8_t* out) {
    uint8_t full[kLdpcK] = {}, cw[kLdpcN];
    for (int j = 0; j < k; j++) full[j] = info[j] & 1;
    ldpcEncode(full, cw);
    for (int j = 0; j < k; j++) out[j] = cw[j];
    for (int j = 0; j < kRmCoded - k; j++) out[k + j] = cw[kLdpcK + j];
}

bool ldpcDecodeRm(const float* llr, int k, uint8_t* info, int maxIter, float alpha) {
    float full[kLdpcN];
    for (int j = 0; j < kLdpcN; j++) full[j] = 0.f;
    for (int j = 0; j < k; j++) full[j] = llr[j];
    for (int j = k; j < kLdpcK; j++) full[j] = 60.f;   // known zeros
    for (int j = 0; j < kRmCoded - k; j++) full[kLdpcK + j] = llr[k + j];
    uint8_t all[kLdpcK];
    // layered schedule (alpha 0.9) since 3.0: about 0.25 dB better at 1 % FER than the flooding one with alpha 0.8 and half the iterations
    // (docs/KAAL-NEXT.md); the alpha argument is kept for the interface but the layered decoder's own value is used
    (void)alpha;
    bool ok = ldpcDecodeLayered(full, all, maxIter, 0.9f, nullptr, nullptr);
    for (int j = k; j < kLdpcK; j++) if (all[j]) ok = false;   // a shortened bit decoded as one: wrong hypothesis
    for (int j = 0; j < k; j++) info[j] = all[j];
    return ok;
}

float ldpcSyndromeScore(const float* llr) {
    const Code& c = code();
    int bad = 0;
    for (int m = 0; m < kLdpcM; m++) {
        int s = 0;
        for (int k = 0; k < c.dc[m]; k++) s ^= llr[c.var[m][k]] < 0;
        bad += s;
    }
    return 1.f - (float)bad / kLdpcM;
}

static uint8_t par(unsigned x) { uint8_t p = 0; while (x) { p ^= 1; x &= x - 1; } return p; }

std::vector<uint8_t> convEncode(const uint8_t* bits, size_t n) {
    static const unsigned g0 = 0171, g1 = 0133;
    std::vector<uint8_t> out(2 * n);
    unsigned state = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned reg = ((unsigned)(bits[i] & 1) << 6) | state;
        out[2 * i] = par(reg & g0);
        out[2 * i + 1] = par(reg & g1);
        state = reg >> 1;
    }
    return out;
}

}  // namespace kaal
}  // namespace dect2
