// Test signals for the third block of 4.0 signal modes (families 752 and up). Generators exist only in the tests; none is reachable from the transmit side.
// Wide-band scenes are made in one-second chunks so that a 2 Msps run does not hold hundreds of megabytes.
#pragma once
#include "x40_gen2.h"
#include <functional>
#include <map>

namespace x40gen {

// Feed a receiver from a chunk generator: gen(t0, n) returns n complex samples starting at time t0.
inline dect2::X40Telemetry runChunks(int variant, double fs, double seconds, const std::function<std::vector<cf32>(double, size_t)>& gen, double offset = 0, double centreHz = 0, double chunkSec = 0.5) {
    dect2::X40Receiver rx; rx.setVariant(variant); rx.configure(fs); rx.setSignalOffset(offset); rx.setCenterHz(centreHz);
    for (double t0 = 0; t0 < seconds - 1e-9; t0 += chunkSec) {
        const size_t n = (size_t)(chunkSec * fs);
        const auto x = gen(t0, n);
        for (size_t i = 0; i < x.size(); i += 4096) rx.feed(x.data() + i, std::min<size_t>(4096, x.size() - i));
    }
    dect2::X40Telemetry t; rx.telemetry(t, 0);
    return t;
}

// A tone with an on / off schedule: on for duty * period of every period, starting at phase0. level: amplitude. drift: Hz per second.
struct SchedTone { double hz = 0, amp = 1, period = 1, duty = 1, phase0 = 0, driftHzPerSec = 0, from = 0, to = 1e9; };
struct ToneScene {
    std::vector<SchedTone> tones; double noisePower = 0; double fs = 1; unsigned seed = 1; std::map<int, double> ph; Rng rng{1};
    explicit ToneScene(double f, double noise, unsigned s = 1) : noisePower(noise), fs(f), seed(s), rng(s) {}
    std::vector<cf32> operator()(double t0, size_t n) {
        std::vector<cf32> x(n, cf32(0, 0));
        for (size_t k = 0; k < tones.size(); k++) {
            const SchedTone& s = tones[k];
            double& p = ph[(int)k];
            for (size_t i = 0; i < n; i++) {
                const double t = t0 + i / fs;
                const double f = s.hz + s.driftHzPerSec * t;
                p += 2 * kPi * f / fs; if (p > 2 * kPi) p -= 2 * kPi; else if (p < -2 * kPi) p += 2 * kPi;
                const double u = std::fmod(t - s.phase0 + 1000 * s.period, s.period);
                if (t >= s.from && t < s.to && u < s.duty * s.period) x[i] += (float)s.amp * cf32((float)std::cos(p), (float)std::sin(p));
            }
        }
        const float sg = (float)std::sqrt(noisePower / 2);
        for (auto& v : x) v += cf32(sg * rng.g(), sg * rng.g());
        return x;
    }
};

// number after the label in a fact, or -999
inline double factNum(const dect2::X40Telemetry& t, const char* key) { for (auto& f : t.facts) if (f.first == key) return std::atof(f.second.c_str()); return -999; }
inline std::string factStr(const dect2::X40Telemetry& t, const char* key) { for (auto& f : t.facts) if (f.first == key) return f.second; return ""; }


// Root-raised-cosine pulse (unit symbol time), roll-off alpha
inline double rrc(double t, double alpha) {
    if (std::fabs(t) < 1e-9) return 1 - alpha + 4 * alpha / kPi;
    if (std::fabs(std::fabs(4 * alpha * t) - 1) < 1e-7) return alpha / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * alpha)) + (1 - 2 / kPi) * std::cos(kPi / (4 * alpha)));
    return (std::sin(kPi * t * (1 - alpha)) + 4 * alpha * t * std::cos(kPi * t * (1 + alpha))) / (kPi * t * (1 - (4 * alpha * t) * (4 * alpha * t)));
}
// M-PSK at symbol rate rs with root-raised-cosine shaping, unit power, at complex rate fs; carrier offset hz with drift, symbol clock error ppm. M = 2, 4 or 8.
// qpskOffset: the constellation sits at pi / M (QPSK at 45 degrees) instead of on the axes.
inline std::vector<cf32> mpsk(int M, double rs, double alpha, double fs, double seconds, double hz = 0, double drift = 0, double ppm = 0, unsigned seed = 1, bool rotated = false) {
    Rng r(seed); const size_t N = (size_t)(fs * seconds); std::vector<cf32> x(N, cf32(0, 0));
    const double T = 1.0 / rs * (1 + ppm * 1e-6); const long nsym = (long)(seconds / T) + 20;
    std::vector<cf32> a((size_t)nsym);
    std::mt19937 g(seed * 7919u + 3);
    for (auto& v : a) { const int k = (int)(g() % (unsigned)M); const double ph = 2 * kPi * k / M + (rotated ? kPi / M : 0); v = cf32((float)std::cos(ph), (float)std::sin(ph)); }
    const int span = 6;
    for (size_t i = 0; i < N; i++) {
        const double t = i / fs / T; const long k0 = (long)std::floor(t);
        cf32 s(0, 0);
        for (long k = k0 - span; k <= k0 + span; k++) { if (k < 0 || k >= nsym) continue; s += a[(size_t)k] * (float)rrc(t - k, alpha); }
        x[i] = s;
    }
    double pw = 0; for (auto& v : x) pw += std::norm(v); pw /= (double)N; const float sc = (float)(1 / std::sqrt(pw)); for (auto& v : x) v *= sc;
    mix(x, hz, drift, fs);
    (void)r;
    return x;
}
// noise power per sample for a stated Es/N0 (dB) of a unit-power signal at symbol rate rs
inline double noiseEsN0(double esn0Db, double rs, double fs) { return fs / (rs * std::pow(10.0, esn0Db / 10)); }

}
