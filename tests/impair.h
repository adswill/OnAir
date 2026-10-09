// Real-world faults applied to a clean test signal, written independently of every receiver, so a test cannot pass only
// because the generator and the receiver share the same assumption. See REAL_WORLD_CHECKLIST.md.
#pragma once
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

namespace impair {
using cf32 = std::complex<float>;

// Turn the whole signal by hz (a tuning error, or a channel that is not centred).
inline void shift(std::vector<cf32>& x, double hz, double rate) {
    const double w = 2 * M_PI * hz / rate;
    for (size_t n = 0; n < x.size(); n++) {
        const double a = w * (double)n;
        x[n] *= cf32((float)std::cos(a), (float)std::sin(a));
    }
}

// A sample clock that is ppm too fast (positive) or too slow: the receiver gets (1 + ppm/1e6) times as many samples per second
// of signal. Windowed-sinc interpolation, 32 taps, good to well below the noise of any test.
inline std::vector<cf32> clock(const std::vector<cf32>& x, double ppm) {
    const double step = 1.0 / (1.0 + ppm * 1e-6);   // input samples per output sample
    const int H = 16;
    std::vector<cf32> y;
    y.reserve((size_t)(x.size() / step) + 1);
    for (double t = H; t < (double)x.size() - H - 1; t += step) {
        const int64_t i = (int64_t)std::floor(t);
        const double f = t - (double)i;
        std::complex<double> acc = 0;
        for (int k = -H + 1; k <= H; k++) {
            const double d = (double)k - f;
            const double s = std::fabs(d) < 1e-12 ? 1.0 : std::sin(M_PI * d) / (M_PI * d);
            const double win = 0.5 + 0.5 * std::cos(M_PI * d / H);   // Hann
            const cf32 v = x[(size_t)(i + k)];
            acc += std::complex<double>(v.real(), v.imag()) * (s * win);
        }
        y.push_back(cf32((float)acc.real(), (float)acc.imag()));
    }
    return y;
}

// I and Q swapped (the spectrum mirrored), as some radios and file formats deliver it.
inline void swapIq(std::vector<cf32>& x) { for (auto& v : x) v = cf32(v.imag(), v.real()); }

// IQ imbalance: the Q branch gainDb stronger than I and phaseDeg off quadrature (a direct-conversion radio's mixer)
inline void iqImbalance(std::vector<cf32>& x, double gainDb, double phaseDeg) {
    const float g = (float)std::pow(10.0, gainDb / 20), sp = (float)std::sin(phaseDeg * M_PI / 180), cp = (float)std::cos(phaseDeg * M_PI / 180);
    for (auto& v : x) v = cf32(v.real(), g * (v.imag() * cp + v.real() * sp));
}

// A DC spike of the given level relative to the signal's RMS.
inline void dc(std::vector<cf32>& x, double relDb) {
    double p = 0;
    for (auto& v : x) p += std::norm(v);
    const float a = (float)std::sqrt(p / (double)std::max<size_t>(1, x.size()) * std::pow(10.0, relDb / 10));
    for (auto& v : x) v += cf32(a, 0);
}

// Scale to a peak level and clip at +-1 on each axis (an overdriven 8-bit radio), then quantise to 8 bits.
inline void clip8(std::vector<cf32>& x, double gain) {
    auto q = [](float v) { v = std::fmax(-1.f, std::fmin(1.f, v)); return std::round(v * 127.f) / 127.f; };
    for (auto& v : x) v = cf32(q(v.real() * (float)gain), q(v.imag() * (float)gain));
}

// An echo delayed by d samples, relDb relative to the direct path (relDb > 0: the echo is the stronger one), turned by phase.
inline void echo(std::vector<cf32>& x, int d, double relDb, double phase = 1.0) {
    const float g = (float)std::pow(10.0, relDb / 20);
    const cf32 r = std::polar(g, (float)phase);
    for (size_t n = x.size(); n-- > (size_t)d;) x[n] += r * x[n - (size_t)d];
}

// White noise for an SNR in dB over the whole sample band.
inline void noise(std::vector<cf32>& x, double snrDb, uint32_t seed = 1) {
    double p = 0;
    for (auto& v : x) p += std::norm(v);
    p /= (double)std::max<size_t>(1, x.size());
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, (float)std::sqrt(p * std::pow(10.0, -snrDb / 10) / 2));
    for (auto& v : x) v += cf32(g(rng), g(rng));
}

// Lost samples: remove n samples starting at sample at (a USB drop).
inline void drop(std::vector<cf32>& x, size_t at, size_t n) {
    if (at >= x.size()) return;
    x.erase(x.begin() + (ptrdiff_t)at, x.begin() + (ptrdiff_t)std::min(x.size(), at + n));
}

// Start in the middle: throw away the first n samples.
inline void skip(std::vector<cf32>& x, size_t n) { x.erase(x.begin(), x.begin() + (ptrdiff_t)std::min(n, x.size())); }
} // namespace impair
