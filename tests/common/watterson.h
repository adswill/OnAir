// Watterson HF channel simulator for tests (ITU-R F.520-2 / Rec. 520-2, the model MIL-STD-188-110 C.6.1 and 188-141 test against):
// independent Rayleigh-fading paths with fixed delays, each tap a complex Gaussian process with a Gaussian Doppler spectrum of the given
// two-sigma width, then optional frequency offset and noise measured in 3 kHz. Written for OnAir's tests, independently of every receiver.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

namespace watterson {
using cf32 = std::complex<float>;

struct Path {
    double delaySec = 0;
    double gainDb = 0;     // average power of the path
};

struct Cfg {
    std::vector<Path> paths;
    double spreadHz = 1.0;     // two-sigma Doppler spread (ITU-R: 2 sigma); 0 or less: fixed paths (no fading)
    double fs = 24000;
    double tapRate = 200;      // rate at which the tap processes are generated, then interpolated to fs
    uint32_t seed = 1;
};

// ITU-R F.520 conditions: two equal paths
inline Cfg ituGood(uint32_t seed = 1) { Cfg c; c.paths = {{0, 0}, {0.5e-3, 0}}; c.spreadHz = 0.1; c.seed = seed; return c; }
inline Cfg ituModerate(uint32_t seed = 1) { Cfg c; c.paths = {{0, 0}, {1e-3, 0}}; c.spreadHz = 0.5; c.seed = seed; return c; }
inline Cfg ituPoor(uint32_t seed = 1) { Cfg c; c.paths = {{0, 0}, {2e-3, 0}}; c.spreadHz = 1.0; c.seed = seed; return c; }

// one tap process of n samples at fs, E|g|^2 = 1
inline std::vector<cf32> tapProcess(size_t n, const Cfg& c, std::mt19937& rng) {
    std::vector<cf32> g(n, cf32(1, 0));
    if (c.spreadHz <= 0) return g;
    // Gaussian Doppler spectrum, sigma = spread / 2: a Gaussian FIR on white noise at tapRate, |H(f)|^2 = exp(-f^2 / (2 sigma^2))
    const double sigF = c.spreadHz / 2, sigT = 1.0 / (2 * M_PI * sigF * std::sqrt(2.0));
    const int half = (int)std::ceil(4 * sigT * c.tapRate);
    std::vector<double> h((size_t)(2 * half + 1));
    double e = 0;
    for (int i = -half; i <= half; i++) { const double t = i / c.tapRate; h[(size_t)(i + half)] = std::exp(-t * t / (2 * sigT * sigT)); e += h[(size_t)(i + half)] * h[(size_t)(i + half)]; }
    for (auto& v : h) v /= std::sqrt(e);
    const size_t nt = (size_t)std::ceil((double)n / c.fs * c.tapRate) + 3;
    std::normal_distribution<double> nd(0.0, std::sqrt(0.5));
    std::vector<std::complex<double>> w(nt + h.size());
    for (auto& v : w) v = {nd(rng), nd(rng)};
    std::vector<std::complex<double>> t(nt);
    for (size_t k = 0; k < nt; k++) { std::complex<double> a = 0; for (size_t j = 0; j < h.size(); j++) a += w[k + j] * h[j]; t[k] = a; }
    for (size_t i = 0; i < n; i++) {
        const double p = (double)i / c.fs * c.tapRate;
        const size_t k = (size_t)p;
        const double f = p - (double)k;
        const std::complex<double> v = t[k] * (1 - f) + t[k + 1] * f;
        g[i] = cf32((float)v.real(), (float)v.imag());
    }
    return g;
}

// the signal through the channel (same length); path delays rounded to the sample; the paths together keep the average power
inline std::vector<cf32> apply(const std::vector<cf32>& x, const Cfg& c) {
    std::mt19937 rng(c.seed);
    std::vector<cf32> y(x.size(), cf32(0, 0));
    double tot = 0;
    for (const Path& p : c.paths) tot += std::pow(10.0, p.gainDb / 10);
    for (const Path& p : c.paths) {
        const size_t d = (size_t)std::lround(p.delaySec * c.fs);
        const float a = (float)std::sqrt(std::pow(10.0, p.gainDb / 10) / std::max(tot, 1e-12));
        const std::vector<cf32> g = tapProcess(x.size(), c, rng);
        for (size_t i = d; i < x.size(); i++) y[i] += x[i - d] * g[i] * a;
    }
    return y;
}

// noise for an average SNR measured in 3 kHz (C.6.1: signal and noise in 3 kHz), sigPower the long-term average signal power
inline void noise3k(std::vector<cf32>& x, double snrDb, double sigPower, double fs = 24000, uint32_t seed = 7) {
    const double n0 = sigPower / std::pow(10.0, snrDb / 10) * (fs / 3000.0);
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, (float)std::sqrt(n0 / 2));
    for (auto& v : x) v += cf32(g(rng), g(rng));
}

inline double power(const std::vector<cf32>& x, size_t from, size_t to) {
    double p = 0;
    for (size_t i = from; i < to && i < x.size(); i++) p += std::norm(x[i]);
    return p / (double)std::max<size_t>(1, to - from);
}

}  // namespace watterson
