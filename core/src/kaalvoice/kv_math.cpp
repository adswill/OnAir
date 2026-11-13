// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Deterministic elementary functions (series in double precision, no libm transcendental calls) and a radix-2 FFT.
#include "kv_internal.h"
#include <cmath>

namespace dect2 {
namespace kaalvoice {
namespace detail {

namespace {
constexpr double kTwoPiHi = 6.28318530717958623200;   // 2 pi split in two parts for the range reduction
constexpr double kTwoPiLo = 2.44929359829470635445e-16;
constexpr double kHalfPi = 1.57079632679489661923;

double reduce(double x) {   // to [-pi, pi]
    const double q = std::floor(x * 0.15915494309189533577 + 0.5);
    return (x - q * kTwoPiHi) - q * kTwoPiLo;
}
double sinSeries(double x) {   // |x| <= pi/2
    const double x2 = x * x;
    double t = x, s = x;
    for (int k = 1; k <= 11; k++) {
        t *= -x2 / (double)((2 * k) * (2 * k + 1));
        s += t;
    }
    return s;
}
}  // namespace

double kvSin(double x) {
    x = reduce(x);
    if (x > kHalfPi) x = kPi - x;
    else if (x < -kHalfPi) x = -kPi - x;
    return sinSeries(x);
}

double kvCos(double x) {
    x = reduce(x);
    return sinSeries(kHalfPi - std::fabs(x));
}

double kvLog2(double x) {
    if (!(x > 0.0)) return -1100.0;
    int e = 0;
    double m = std::frexp(x, &e);   // exact
    if (m < 0.70710678118654752440) { m *= 2.0; e -= 1; }
    const double s = (m - 1.0) / (m + 1.0), s2 = s * s;
    double t = s, sum = s;
    for (int k = 1; k <= 12; k++) {
        t *= s2;
        sum += t / (double)(2 * k + 1);
    }
    return (double)e + 2.0 * sum * 1.44269504088896340736;
}

double kvExp2(double x) {
    if (x < -1070.0) return 0.0;
    if (x > 1020.0) x = 1020.0;
    const double n = std::floor(x);
    const double y = (x - n) * 0.69314718055994530942;   // [0, ln 2)
    double t = 1.0, s = 1.0;
    for (int k = 1; k <= 20; k++) {
        t *= y / (double)k;
        s += t;
    }
    return std::ldexp(s, (int)n);   // exact
}

void fft(float* re, float* im, int n, bool inverse) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = (inverse ? 2.0 : -2.0) * kPi / len;
        const int h = len / 2;
        for (int k = 0; k < h; k++) {
            const float wr = (float)kvCos(ang * k), wi = (float)kvSin(ang * k);
            for (int i = k; i < n; i += len) {
                const int j = i + h;
                const float xr = re[j] * wr - im[j] * wi, xi = re[j] * wi + im[j] * wr;
                re[j] = re[i] - xr; im[j] = im[i] - xi;
                re[i] += xr; im[i] += xi;
            }
        }
    }
}

}  // namespace detail
}  // namespace kaalvoice
}  // namespace dect2
