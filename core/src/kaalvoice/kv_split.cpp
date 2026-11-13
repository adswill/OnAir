// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Band split for 16 kHz speech: a 95-tap Kaiser-windowed low-pass (cut-off 3.75 kHz) decimates to the 8 kHz core and interpolates
// back. The encoder takes the high band as the exact remainder x - up(down(x)), so the two paths add up to the delayed input.
#include "kv_internal.h"
#include <cmath>

namespace dect2 {
namespace kaalvoice {
namespace detail {

namespace {
double besselI0(double x) {
    double s = 1.0, t = 1.0;
    const double q = x * x * 0.25;
    for (int k = 1; k < 40; k++) {
        t *= q / ((double)k * k);
        s += t;
    }
    return s;
}
struct Taps {
    float h[kSplitTaps];
    Taps() {
        const int M = kSplitTaps - 1;
        const double fc = 3750.0 / 16000.0, beta = 5.0;
        double hd[kSplitTaps], sum = 0.0;
        for (int k = 0; k <= M; k++) {
            const double t = k - M * 0.5;
            const double x = 2.0 * kPi * fc * t;
            const double sinc = t == 0.0 ? 2.0 * fc : kvSin(x) / (kPi * t);
            const double u = 2.0 * k / M - 1.0;
            const double r = 1.0 - u * u;
            hd[k] = sinc * besselI0(beta * std::sqrt(r > 0.0 ? r : 0.0)) / besselI0(beta);
            sum += hd[k];
        }
        for (int k = 0; k <= M; k++) h[k] = (float)(hd[k] / sum);
    }
};
}  // namespace

const float* splitTaps() {
    static const Taps t;
    return t.h;
}

void Down2::run(const float* x, int n16, float* y8) {
    const float* h = splitTaps();
    constexpr int T = kSplitTaps;
    // hist[0..T-1]: the last T input samples, newest at hist[T-1]
    for (int m = 0; m < n16 / 2; m++) {
        for (int j = 0; j < T - 2; j++) hist[j] = hist[j + 2];
        hist[T - 2] = x[2 * m];
        hist[T - 1] = x[2 * m + 1];
        float s = 0.0f;
        for (int k = 0; k < T; k++) s += h[k] * hist[T - 1 - k];
        y8[m] = s;
    }
}

void Up2::run(const float* x8, int n8, float* y16) {
    const float* h = splitTaps();
    constexpr int H = kSplitTaps / 2 + 1;   // 48 input samples
    for (int m = 0; m < n8; m++) {
        for (int j = 0; j < H - 1; j++) hist[j] = hist[j + 1];
        hist[H - 1] = x8[m];
        float s0 = 0.0f, s1 = 0.0f;
        for (int i = 0; i < H; i++) {
            const float v = hist[H - 1 - i];
            s0 += h[2 * i] * v;
            if (2 * i + 1 < kSplitTaps) s1 += h[2 * i + 1] * v;
        }
        y16[2 * m] = 2.0f * s0;
        y16[2 * m + 1] = 2.0f * s1;
    }
}

}  // namespace detail
}  // namespace kaalvoice
}  // namespace dect2
