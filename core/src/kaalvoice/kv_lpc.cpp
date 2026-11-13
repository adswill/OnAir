// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LPC analysis, line spectral frequencies and the direct-form filters of the 8 kHz core.
#include "kv_internal.h"
#include <cmath>

namespace dect2 {
namespace kaalvoice {
namespace detail {

void autocorr(const float* x, const float* win, int n, int p, double* r) {
    float xw[640];
    for (int i = 0; i < n; i++) xw[i] = win ? x[i] * win[i] : x[i];
    for (int k = 0; k <= p; k++) {
        double s = 0.0;
        for (int i = k; i < n; i++) s += (double)xw[i] * (double)xw[i - k];
        r[k] = s;
    }
}

void lagWindow(double* r, int p) {
    static double w[17];
    static bool init = false;
    if (!init) {   // exp(-0.5 (2 pi f0 k / fs)^2), f0 = 60 Hz, fs = 8 kHz
        for (int k = 0; k <= 16; k++) {
            const double a = 2.0 * kPi * 60.0 * k / 8000.0;
            w[k] = kvExp(-0.5 * a * a);
        }
        init = true;
    }
    r[0] = r[0] * 1.0001 + 1e-3;
    for (int k = 1; k <= p; k++) r[k] *= w[k];
}

bool levinson(const double* r, int p, float* a) {
    double aa[17] = {1.0}, tmp[17];
    for (int i = 1; i <= p; i++) aa[i] = 0.0;
    double err = r[0];
    bool ok = err > 0.0;
    for (int i = 1; ok && i <= p; i++) {
        double acc = r[i];
        for (int j = 1; j < i; j++) acc += aa[j] * r[i - j];
        const double k = -acc / err;
        if (!(k > -0.9995 && k < 0.9995)) { ok = false; break; }
        for (int j = 1; j < i; j++) tmp[j] = aa[j] + k * aa[i - j];
        for (int j = 1; j < i; j++) aa[j] = tmp[j];
        aa[i] = k;
        err *= (1.0 - k * k);
        if (!(err > 0.0)) ok = false;
    }
    if (!ok) {   // fall back to a flat filter (the caller keeps the previous LSFs)
        a[0] = 1.0f;
        for (int i = 1; i <= p; i++) a[i] = 0.0f;
        return false;
    }
    for (int i = 0; i <= p; i++) a[i] = (float)aa[i];
    return true;
}

namespace {
// Chebyshev series value of the sum (sel 0) or difference (sel 1) polynomial at x = cos(w).
double chebEval(const double* f, double x) {   // f[0..5]: c5 = f0 .. c0 = f5 / 2
    double b0 = 0.0, b1 = 0.0, b2 = 0.0;
    const double c[6] = {f[5] * 0.5, f[4], f[3], f[2], f[1], f[0]};
    for (int k = 5; k >= 1; k--) {
        b0 = 2.0 * x * b1 - b2 + c[k];
        b2 = b1;
        b1 = b0;
    }
    return x * b1 - b2 + c[0];
}
constexpr int kGrid = 200;
const double* gridCos() {
    static double g[kGrid + 1];
    static bool init = false;
    if (!init) {
        for (int i = 0; i <= kGrid; i++) g[i] = kvCos(kPi * i / kGrid);
        init = true;
    }
    return g;
}
}  // namespace

bool lpcToLsf(const float* a, double* lsf) {
    double f1[6], f2[6];
    f1[0] = f2[0] = 1.0;
    for (int i = 0; i < 5; i++) {
        f1[i + 1] = (double)a[i + 1] + (double)a[kP - i] - f1[i];
        f2[i + 1] = (double)a[i + 1] - (double)a[kP - i] + f2[i];
    }
    const double* g = gridCos();
    int found = 0;
    const double* fs[2] = {f1, f2};
    double prev[2] = {chebEval(f1, g[0]), chebEval(f2, g[0])};
    double roots[2][5];
    int nr[2] = {0, 0};
    for (int i = 1; i <= kGrid; i++) {
        for (int s = 0; s < 2; s++) {
            const double v = chebEval(fs[s], g[i]);
            if (nr[s] < 5 && ((prev[s] <= 0.0 && v > 0.0) || (prev[s] >= 0.0 && v < 0.0))) {
                double lo = kPi * (i - 1) / kGrid, hi = kPi * i / kGrid, vlo = prev[s];
                for (int it = 0; it < 10; it++) {
                    const double mid = 0.5 * (lo + hi);
                    const double vm = chebEval(fs[s], kvCos(mid));
                    if ((vm <= 0.0) == (vlo <= 0.0)) { lo = mid; vlo = vm; }
                    else hi = mid;
                }
                roots[s][nr[s]++] = 0.5 * (lo + hi);
            }
            prev[s] = v;
        }
    }
    if (nr[0] != 5 || nr[1] != 5) return false;
    for (int i = 0; i < 5; i++) {
        lsf[2 * i] = roots[0][i];
        lsf[2 * i + 1] = roots[1][i];
        found += 2;
    }
    for (int i = 1; i < kP; i++)
        if (!(lsf[i] > lsf[i - 1])) return false;
    return found == kP;
}

void lsfToLpc(const double* lsf, float* a) {
    double p[12] = {1.0}, q[12] = {1.0};
    int np = 0, nq = 0;
    for (int i = 0; i < 5; i++) {
        for (int s = 0; s < 2; s++) {
            double* f = s ? q : p;
            int& n = s ? nq : np;
            const double c = -2.0 * kvCos(lsf[2 * i + s]);
            // multiply by (1 + c z^-1 + z^-2)
            f[n + 1] = 0.0;
            f[n + 2] = 0.0;
            for (int j = n + 2; j >= 1; j--) {
                f[j] += c * f[j - 1] + (j >= 2 ? f[j - 2] : 0.0);
            }
            n += 2;
        }
    }
    // P = p (1 + z^-1), Q = q (1 - z^-1)
    double P[12], Q[12];
    for (int j = 0; j <= kP + 1; j++) {
        const double pj = j <= kP ? p[j] : 0.0, pj1 = j >= 1 ? p[j - 1] : 0.0;
        const double qj = j <= kP ? q[j] : 0.0, qj1 = j >= 1 ? q[j - 1] : 0.0;
        P[j] = pj + pj1;
        Q[j] = qj - qj1;
    }
    a[0] = 1.0f;
    for (int j = 1; j <= kP; j++) a[j] = (float)(0.5 * (P[j] + Q[j]));
}

void weightLpc(const float* a, double g, float* out) {
    double f = 1.0;
    out[0] = a[0];
    for (int i = 1; i <= kP; i++) {
        f *= g;
        out[i] = (float)(a[i] * f);
    }
}

void lsfStabilise(double* lsf, double minGap) {
    for (int i = 1; i < kP; i++)   // insertion sort (bit errors can reorder)
        for (int j = i; j > 0 && lsf[j] < lsf[j - 1]; j--) { const double t = lsf[j]; lsf[j] = lsf[j - 1]; lsf[j - 1] = t; }
    if (lsf[0] < minGap) lsf[0] = minGap;
    for (int i = 1; i < kP; i++)
        if (lsf[i] < lsf[i - 1] + minGap) lsf[i] = lsf[i - 1] + minGap;
    const double top = kPi - minGap;
    if (lsf[kP - 1] > top) lsf[kP - 1] = top;
    for (int i = kP - 2; i >= 0; i--)
        if (lsf[i] > lsf[i + 1] - minGap) lsf[i] = lsf[i + 1] - minGap;
}

void firA(const float* a, const float* x, float* y, int n, float* mem) {
    for (int i = 0; i < n; i++) {
        const float xi = x[i];
        float s = xi;
        for (int k = 1; k <= kP; k++) s += a[k] * mem[k - 1];
        for (int k = kP - 1; k > 0; k--) mem[k] = mem[k - 1];
        mem[0] = xi;
        y[i] = s;
    }
}

void iirA(const float* a, const float* x, float* y, int n, float* mem) {
    for (int i = 0; i < n; i++) {
        float s = x[i];
        for (int k = 1; k <= kP; k++) s -= a[k] * mem[k - 1];
        for (int k = kP - 1; k > 0; k--) mem[k] = mem[k - 1];
        mem[0] = s;
        y[i] = s;
    }
}

}  // namespace detail
}  // namespace kaalvoice
}  // namespace dect2
