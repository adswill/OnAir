// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pyramid vector quantiser (Fischer 1986): enumeration of the integer vectors of length n with sum |y_i| = k, and the analysis-by-
// synthesis pulse search of the fixed codebook. The enumeration orders, at each position, the value 0 first, then +1, -1, +2, -2 ...
#include "kv_internal.h"
#include <cstdint>

namespace dect2 {
namespace kaalvoice {
namespace detail {

namespace {
constexpr int kMaxN = 81, kMaxK = 64;
struct Table {
    uint64_t v[kMaxN + 1][kMaxK + 1];
    Table() {
        for (int n = 0; n <= kMaxN; n++)
            for (int k = 0; k <= kMaxK; k++) {
                if (k == 0) { v[n][k] = 1; continue; }
                if (n == 0) { v[n][k] = 0; continue; }
                const uint64_t a = v[n - 1][k], b = v[n][k - 1], c = v[n - 1][k - 1];
                uint64_t s = a + b;
                if (s < a) s = UINT64_MAX;
                const uint64_t t = s + c;
                v[n][k] = (t < s || s == UINT64_MAX) ? UINT64_MAX : t;
            }
    }
};
const Table& table() { static const Table t; return t; }
inline uint64_t V(int n, int k) { return (n < 0 || k < 0 || n > kMaxN || k > kMaxK) ? 0 : table().v[n][k]; }
}  // namespace

uint64_t pvqCount(int n, int k) { return V(n, k); }

int pvqBits(int n, int k) {
    const uint64_t c = V(n, k);
    if (c == 0 || c > (1ull << 63)) return 99;
    int b = 0;
    while (b < 63 && (1ull << b) < c) b++;
    return b;
}

int pvqMaxK(int n, int bits) {
    int k = 0;
    while (k + 1 <= kMaxK && pvqBits(n, k + 1) <= bits) k++;
    return k;
}

uint64_t pvqIndex(const int* y, int n, int k) {
    uint64_t idx = 0;
    for (int i = 0; i < n && k > 0; i++) {
        const int rem = n - i - 1;   // positions after i
        const int a = y[i] < 0 ? -y[i] : y[i];
        if (a == 0) continue;        // value 0 comes first: nothing to skip
        idx += V(rem, k);            // all vectors with y_i = 0
        for (int j = 1; j < a; j++) idx += 2 * V(rem, k - j);
        if (y[i] < 0) idx += V(rem, k - a);
        k -= a;
    }
    return idx;
}

void pvqVector(uint64_t idx, int n, int k, int* y) {
    const uint64_t total = V(n, k);
    if (total) idx %= total;
    for (int i = 0; i < n; i++) {
        y[i] = 0;
        const int rem = n - i - 1;
        if (k == 0) continue;
        if (i == n - 1) { // the last position takes what is left
            const uint64_t sign = idx;   // 0 -> +k, 1 -> -k
            y[i] = sign ? -k : k;
            k = 0;
            continue;
        }
        uint64_t c = V(rem, k);
        if (idx < c) continue;
        idx -= c;
        int a = 1;
        for (; a <= k; a++) {
            c = V(rem, k - a);
            if (idx < c) { y[i] = a; break; }
            idx -= c;
            if (idx < c) { y[i] = -a; break; }
            idx -= c;
        }
        if (a > k) { y[i] = k; a = k; }   // cannot happen for a valid index
        k -= a;
    }
}

void pvqSearch(const double* d, const double* phi, int n, int k, int* y) {
    double g[128];   // (Phi y)_i
    for (int i = 0; i < n; i++) { y[i] = 0; g[i] = 0.0; }
    if (k <= 0) return;
    double C = 0.0, E = 0.0;
    auto addAt = [&](int i, int s) {
        const double* row = phi + (size_t)i * n;
        for (int j = 0; j < n; j++) g[j] += s * row[j];
    };
    for (int p = 0; p < k; p++) {
        int best = 0;
        double bn = -1.0, bd = 1.0;
        for (int i = 0; i < n; i++) {
            const double ad = d[i] < 0 ? -d[i] : d[i];
            const int s = d[i] < 0 ? -1 : 1;
            const double c = C + ad, e = E + 2.0 * s * g[i] + phi[(size_t)i * n + i];
            if (e <= 0.0) continue;
            if (c * c * bd > bn * e) { bn = c * c; bd = e; best = i; }
        }
        const int s = d[best] < 0 ? -1 : 1;
        C += d[best] < 0 ? -d[best] : d[best];
        E += 2.0 * s * g[best] + phi[(size_t)best * n + best];
        y[best] += s;
        addAt(best, s);
    }
    // refinement: move single pulses to a better place while the criterion improves
    for (int pass = 0; pass < 2; pass++) {
        bool moved = false;
        for (int i = 0; i < n; i++) {
            if (y[i] == 0) continue;
            const int s = y[i] < 0 ? -1 : 1;
            const double ad = d[i] < 0 ? -d[i] : d[i];
            // remove one unit at i
            const double c0 = C - ad, e0 = E - 2.0 * s * g[i] + phi[(size_t)i * n + i];
            y[i] -= s;
            addAt(i, -s);
            int best = i;
            double bn = (c0 + ad) * (c0 + ad), bd = E;   // putting it back = the current state
            for (int j = 0; j < n; j++) {
                const int sj = d[j] < 0 ? -1 : 1;
                const double adj = d[j] < 0 ? -d[j] : d[j];
                const double c = c0 + adj, e = e0 + 2.0 * sj * g[j] + phi[(size_t)j * n + j];
                if (e <= 0.0) continue;
                if (c * c * bd > bn * e * (1.0 + 1e-9)) { bn = c * c; bd = e; best = j; }
            }
            const int sb = d[best] < 0 ? -1 : 1;
            C = c0 + (d[best] < 0 ? -d[best] : d[best]);
            E = e0 + 2.0 * sb * g[best] + phi[(size_t)best * n + best];
            y[best] += sb;
            addAt(best, sb);
            if (best != i) moved = true;
        }
        if (!moved) break;
    }
}

}  // namespace detail
}  // namespace kaalvoice
}  // namespace dect2
