// The filters the HF digital receiver (hfdig_rx.cpp) and its test signal (hfdig_gen.cpp) share: a Kaiser-windowed low-pass, and from it
// the complex band-pass that keeps the upper sideband audio, 200 .. 3800 Hz above 0 Hz.
#pragma once
#include "dect2/ring.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace dect2 {
namespace hfdig {

constexpr double kPi = 3.14159265358979323846;
constexpr double kUsbLowHz = 200, kUsbHighHz = 3800;

inline double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; }
    return s;
}

// n taps, cut-off fc in cycles per sample, unity gain at 0 Hz
inline std::vector<double> lowpass(int n, double fc, double beta) {
    std::vector<double> h((size_t)n);
    const double c = (n - 1) * 0.5;
    double sum = 0;
    for (int k = 0; k < n; k++) {
        const double t = k - c;
        const double sinc = t == 0 ? 2 * fc : std::sin(2 * kPi * fc * t) / (kPi * t);
        const double r = c > 0 ? t / c : 0;
        const double w = besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / besselI0(beta);
        h[(size_t)k] = sinc * w;
        sum += h[(size_t)k];
    }
    for (auto& v : h) v /= sum;
    return h;
}

// Complex band-pass for y[n] = sum h[k] x[n-k] at `rate`: passes kUsbLowHz .. kUsbHighHz, stops the lower sideband (negative frequencies)
inline std::vector<cf32> usbTaps(int n, double rate) {
    const auto lp = lowpass(n, 0.5 * (kUsbHighHz - kUsbLowHz) / rate, 7.0);
    const double w0 = 2 * kPi * 0.5 * (kUsbHighHz + kUsbLowHz) / rate;
    const double c = (n - 1) * 0.5;
    std::vector<cf32> h((size_t)n);
    for (int k = 0; k < n; k++) h[(size_t)k] = cf32((float)(lp[(size_t)k] * std::cos(w0 * (k - c))), (float)(lp[(size_t)k] * std::sin(w0 * (k - c))));
    return h;
}

} // namespace hfdig
} // namespace dect2
