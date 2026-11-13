// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Antenna designer: dense complex LU with partial pivoting (float64), right-looking and blocked; the trailing update of a large matrix is
// split over threads by rows. Row-major storage; whole rows are swapped at each pivot (as LAPACK getrf), so luSolve applies piv in order.
#include "dect2/antd/antd.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace dect2::antd {

namespace {
constexpr int kNb = 48;   // block width

inline double mag1(const cd& z) { return std::fabs(z.real()) + std::fabs(z.imag()); }

// rows [r0, r1) of the trailing matrix: a[i][c0..n) -= sum_p a[i][p] * a[p][c0..n), p in [k0, k1)
void trailing(cd* a, int n, int k0, int k1, int c0, int r0, int r1) {
    const int w = n - c0;
    if (w <= 0) return;
    for (int i = r0; i < r1; ++i) {
        double* ri = reinterpret_cast<double*>(a + (size_t)i * n + c0);
        for (int p = k0; p < k1; ++p) {
            const cd l = a[(size_t)i * n + p];
            if (l.real() == 0 && l.imag() == 0) continue;
            const double lr = l.real(), li = l.imag();
            const double* rp = reinterpret_cast<const double*>(a + (size_t)p * n + c0);
            for (int j = 0; j < w; ++j) {
                const double ur = rp[2 * j], ui = rp[2 * j + 1];
                ri[2 * j] -= lr * ur - li * ui;
                ri[2 * j + 1] -= lr * ui + li * ur;
            }
        }
    }
}
} // namespace

bool luFactor(std::vector<cd>& av, int n, std::vector<int>& piv, int threads) {
    piv.assign(n, 0);
    if (n <= 0) return true;
    if ((int)av.size() < n * n) return false;
    cd* a = av.data();
    if (threads <= 0) threads = (int)std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
    for (int k0 = 0; k0 < n; k0 += kNb) {
        const int k1 = std::min(n, k0 + kNb);
        // the panel: columns k0..k1, all rows below
        for (int k = k0; k < k1; ++k) {
            int p = k; double best = mag1(a[(size_t)k * n + k]);
            for (int i = k + 1; i < n; ++i) { const double v = mag1(a[(size_t)i * n + k]); if (v > best) { best = v; p = i; } }
            piv[k] = p;
            if (best == 0) return false;
            if (p != k) std::swap_ranges(a + (size_t)k * n, a + (size_t)k * n + n, a + (size_t)p * n);
            const cd inv = 1.0 / a[(size_t)k * n + k];
            for (int i = k + 1; i < n; ++i) {
                cd& l = a[(size_t)i * n + k];
                l *= inv;
                if (l.real() == 0 && l.imag() == 0) continue;
                for (int j = k + 1; j < k1; ++j) a[(size_t)i * n + j] -= l * a[(size_t)k * n + j];
            }
        }
        if (k1 >= n) break;
        // U12 = L11^-1 A12
        for (int k = k0; k < k1; ++k)
            for (int i = k + 1; i < k1; ++i) {
                const cd l = a[(size_t)i * n + k];
                if (l.real() == 0 && l.imag() == 0) continue;
                for (int j = k1; j < n; ++j) a[(size_t)i * n + j] -= l * a[(size_t)k * n + j];
            }
        // A22 -= L21 U12
        const int rows = n - k1;
        const int t = (rows >= 160 && threads > 1) ? std::min(threads, rows / 64) : 1;
        if (t <= 1) {
            trailing(a, n, k0, k1, k1, k1, n);
        } else {
            std::vector<std::thread> th;
            th.reserve(t);
            for (int q = 0; q < t; ++q) {
                const int r0 = k1 + (int)((long long)rows * q / t), r1 = k1 + (int)((long long)rows * (q + 1) / t);
                th.emplace_back([=] { trailing(a, n, k0, k1, k1, r0, r1); });
            }
            for (auto& x : th) x.join();
        }
    }
    return true;
}

void luSolve(const std::vector<cd>& lu, int n, const std::vector<int>& piv, std::vector<cd>& b) {
    if ((int)b.size() < n) return;
    for (int k = 0; k < n; ++k) if (piv[k] != k) std::swap(b[k], b[piv[k]]);
    for (int i = 0; i < n; ++i) {
        cd s = b[i];
        const cd* r = lu.data() + (size_t)i * n;
        for (int j = 0; j < i; ++j) s -= r[j] * b[j];
        b[i] = s;
    }
    for (int i = n - 1; i >= 0; --i) {
        cd s = b[i];
        const cd* r = lu.data() + (size_t)i * n;
        for (int j = i + 1; j < n; ++j) s -= r[j] * b[j];
        b[i] = s / r[i];
    }
}

} // namespace dect2::antd
