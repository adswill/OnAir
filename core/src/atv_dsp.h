// Small DSP helpers for the analog TV receiver and generator: FIR design, a phase accumulator, block filters, a table-driven noise source.
#pragma once
#include "dect2/ring.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dect2 {
namespace atvdsp {

constexpr double kPi = 3.14159265358979323846;

inline double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 60; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; if (t < 1e-14 * s) break; }
    return s;
}

// Kaiser window shape parameter for a stopband attenuation in dB
inline double kaiserBeta(double a) { return a > 50 ? 0.1102 * (a - 8.7) : a > 21 ? 0.5842 * std::pow(a - 21, 0.4) + 0.07886 * (a - 21) : 0.0; }

inline double kaiser(double x, double beta) {       // x in [-1, 1]
    const double r = 1 - x * x;
    return r <= 0 ? 0.0 : besselI0(beta * std::sqrt(r)) / besselI0(beta);
}

// Linear-phase low-pass: unity gain at DC, passband edge fpass, stopband edge fstop (Hz), length from the Kaiser formula (odd, at most maxTaps)
inline std::vector<float> lowpass(double fpass, double fstop, double fs, double atten = 60, int maxTaps = 401) {
    fstop = std::min(fstop, fs * 0.5 * 0.999);
    const double fc = 0.5 * (fpass + fstop) / fs, df = std::max((fstop - fpass) / fs, 1e-4);
    int n = (int)std::ceil((atten - 7.95) / (14.36 * df)) + 1;
    n = std::min(maxTaps, std::max(5, n)) | 1;
    const double beta = kaiserBeta(atten);
    std::vector<float> h((size_t)n);
    const int m = n / 2;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        const double x = i - m;
        const double s = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
        h[(size_t)i] = (float)(s * kaiser(x / (m + 0.5), beta));
        sum += h[(size_t)i];
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}

// FIR from a sampled frequency response D(f) (complex, f in Hz in [-fs/2, fs/2)), n taps (odd), Kaiser window.
// Result tap k (k = 0 .. n-1) is the response at lag k - n/2.
template <class F>
std::vector<std::complex<double>> fromResponse(F resp, double fs, int n, double beta) {
    const int grid = 8192;
    std::vector<std::complex<double>> d((size_t)grid), h((size_t)n);
    for (int k = 0; k < grid; k++) {
        const double f = (k < grid / 2 ? k : k - grid) * fs / grid;
        d[(size_t)k] = resp(f);
    }
    const int m = n / 2;
    for (int i = 0; i < n; i++) {
        const int lag = i - m;
        std::complex<double> acc = 0;
        for (int k = 0; k < grid; k++) {
            const double ph = 2 * kPi * (double)k * (double)lag / grid;
            acc += d[(size_t)k] * std::complex<double>(std::cos(ph), std::sin(ph));
        }
        h[(size_t)i] = acc / (double)grid * kaiser((double)lag / (m + 0.5), beta);
    }
    return h;
}

// sin(x) for |x| <= pi/2 (error below 1e-7) and for |x| <= pi (folded): plain arithmetic, so that loops over them vectorise
inline float sinHalf(float x) {
    const float x2 = x * x;
    return x * (1.f + x2 * (-1.6666667e-1f + x2 * (8.3333338e-3f + x2 * (-1.9841270e-4f + x2 * 2.7557319e-6f))));
}
inline float sinPi(float x) {
    x = x > 1.5707963f ? 3.1415927f - x : x < -1.5707963f ? -3.1415927f - x : x;
    return sinHalf(x);
}

// atan2 in float, a polynomial on [0, 1] (error below 1e-5 rad), about ten times faster than the library's
inline float fastAtan2(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float mx = std::max(ax, ay), mn = std::min(ax, ay);
    if (mx == 0.f) return 0.f;
    const float a = mn / mx, s = a * a;
    float r = a + a * s * (-0.327622764f + s * (0.15931422f - 0.0464964749f * s));
    if (ay > ax) r = 1.57079632679f - r;
    if (x < 0) r = 3.14159265359f - r;
    return y < 0 ? -r : r;
}

// Phase accumulator in double precision: a phasor that is advanced without sin/cos per sample.
class Nco {
public:
    void setFreq(double hz, double fs) { w_ = std::polar(1.0, 2 * kPi * hz / fs); }
    void setPhase(double rad) { p_ = std::polar(1.0, rad); }
    void reset() { p_ = 1.0; n_ = 0; }
    std::complex<double> phasor() const { return p_; }
    void step() { p_ *= w_; if ((++n_ & 1023) == 0) p_ /= std::abs(p_); }
    void advance(size_t n) { for (size_t i = 0; i < n; i++) step(); }
private:
    std::complex<double> p_{1.0, 0.0}, w_{1.0, 0.0};
    uint32_t n_ = 0;
};

// xorshift64* and a table-driven Gaussian source (fast: two table reads per complex sample). The table is long enough that the repeat
// (about 0.1 s at 10 Msps for each of the two tables, a different length each) is not visible in a picture or audible in the sound.
class NoiseSource {
public:
    explicit NoiseSource(uint64_t seed = 1) {
        s_ = seed * 0x9E3779B97F4A7C15ull + 0x1234567ull;
        if (!s_) s_ = 1;
        a_.resize(kA); b_.resize(kB);
        fill(a_); fill(b_);
    }
    // adds sigma * (unit-variance complex Gaussian) to out[i]; sigma is the rms of the complex sample
    void add(cf32* out, size_t n, float sigma) {
        const float k = sigma / 1.41421356f;     // sigma per axis
        for (size_t i = 0; i < n; i++) {
            const cf32 v = (a_[ia_] + b_[ib_]) * (k * 0.70710678f);
            out[i] += v;
            if (++ia_ == kA) ia_ = 0;
            if (++ib_ == kB) ib_ = 0;
        }
    }
    float uniform() { return (float)((next() >> 11) * (1.0 / 9007199254740992.0)); }
private:
    static constexpr size_t kA = 262144, kB = 262139;
    uint64_t next() { s_ ^= s_ >> 12; s_ ^= s_ << 25; s_ ^= s_ >> 27; return s_ * 0x2545F4914F6CDD1Dull; }
    void fill(std::vector<cf32>& t) {
        double e = 0;
        for (auto& v : t) {                       // Box-Muller, once
            const double u1 = (double)((next() >> 11) + 1) * (1.0 / 9007199254740993.0), u2 = (double)(next() >> 11) * (1.0 / 9007199254740992.0);
            const double r = std::sqrt(-2 * std::log(u1));
            v = cf32((float)(r * std::cos(2 * kPi * u2)), (float)(r * std::sin(2 * kPi * u2)));
            e += std::norm(v);
        }
        const float g = (float)std::sqrt(2.0 / (e / (double)t.size()));   // unit variance per axis
        for (auto& v : t) v *= g;
    }
    uint64_t s_;
    std::vector<cf32> a_, b_;
    size_t ia_ = 0, ib_ = 0;
};

} // namespace atvdsp
} // namespace dect2
