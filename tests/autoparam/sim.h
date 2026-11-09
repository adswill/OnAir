// Channel impairments for the Auto tests: everything on 8 kHz real audio. A carrier offset and slow drift are applied to the whole signal (analytic
// signal, multiply, keep the real part), so the tones move as they do on air; noise is added last, measured against the signal itself.
#pragma once
#include "dect2/dsp/fftutil.h"
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace dect2::autotest {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 2654435761ull + 7) {}
    double u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return ((s >> 11) + 0.5) / 9007199254740992.0; }
    double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); }
};

inline std::vector<std::complex<float>> analytic(const std::vector<float>& a) {
    int N = 1;
    while ((size_t)N < a.size()) N <<= 1;
    Fft f(N);
    std::vector<cf32> x((size_t)N, cf32(0, 0));
    for (size_t i = 0; i < a.size(); i++) x[i] = cf32(a[i], 0);
    f.forward(x.data());
    x[0] *= 1.f;
    for (int i = 1; i < N / 2; i++) x[(size_t)i] *= 2.f;
    for (int i = N / 2 + 1; i < N; i++) x[(size_t)i] = cf32(0, 0);
    f.inverse(x.data());
    for (auto& v : x) v /= (float)N;
    x.resize(a.size());
    return x;
}

// offset in Hz, drift in Hz per minute over the buffer, in place
inline void shift(std::vector<float>& a, double fs, double offsetHz, double driftHzPerMin) {
    if (offsetHz == 0 && driftHzPerMin == 0) return;
    auto z = analytic(a);
    double ph = 0;
    for (size_t i = 0; i < a.size(); i++) {
        const double f = offsetHz + driftHzPerMin / 60.0 * ((double)i / fs);
        ph += 2 * 3.14159265358979323846 * f / fs;
        const std::complex<float> y = z[i] * std::complex<float>((float)std::cos(ph), (float)std::sin(ph));
        a[i] = y.real();
    }
}

// noise so that signal power / noise power in `bwHz` is snrDb; returns nothing, in place
inline void addNoise(std::vector<float>& a, double fs, double snrDb, double bwHz, uint64_t seed) {
    double p = 0;
    for (float v : a) p += (double)v * v;
    p /= (double)a.size();
    if (p <= 0) p = 0.125;
    const double noiseIn3k = p / std::pow(10.0, snrDb / 10.0);
    const double sigma = std::sqrt(noiseIn3k * (fs / 2.0) / bwHz);
    Rng r(seed);
    for (auto& v : a) v += (float)(sigma * r.g());
}

inline std::vector<float> noiseOnly(double fs, double seconds, double sigma, uint64_t seed) {
    std::vector<float> a((size_t)(seconds * fs));
    Rng r(seed);
    for (auto& v : a) v = (float)(sigma * r.g());
    return a;
}

}  // namespace dect2::autotest
