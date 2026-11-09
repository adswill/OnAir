// Test signals of the radio-control and mesh link tests: GFSK bursts and LoRa frames written straight into a sample buffer. Only for the tests: the app
// has no way to transmit these.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/mesh/mesh_lora.h"
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

namespace rctest {
using dect2::cf32;

// Adds a GFSK (BT 0.5) burst of `bits` at carrier offsetHz with deviation devHz, starting at sample `start`
inline void addGfsk(std::vector<cf32>& x, size_t start, const std::vector<uint8_t>& bits, double rate, double devHz, double fs, double offsetHz, float amp) {
    const double sps = fs / rate, pulseSym = 3.0;
    const int half = (int)(pulseSym * sps / 2);
    std::vector<double> h(2 * half + 1);
    const double a = std::sqrt(std::log(2.0)) / (0.5 / rate * 2.0);
    double sum = 0;
    for (int i = -half; i <= half; i++) { const double t = i / fs; h[i + half] = std::exp(-a * a * t * t * M_PI * M_PI); sum += h[i + half]; }
    for (double& v : h) v /= sum;
    const size_t n = (size_t)(bits.size() * sps) + 2 * half + 2;
    std::vector<double> nrz(n, 0.0), freq(n, 0.0);
    for (size_t b = 0; b < bits.size(); b++) {
        const size_t i0 = (size_t)(b * sps) + half, i1 = (size_t)((b + 1) * sps) + half;
        for (size_t i = i0; i < i1 && i < n; i++) nrz[i] = bits[b] ? 1.0 : -1.0;
    }
    for (size_t i = half; i + half < n; i++) {
        double s = 0;
        for (int k = -half; k <= half; k++) s += nrz[i + k] * h[k + half];
        freq[i] = s;
    }
    double ph = 0;
    for (size_t i = 0; i < n && start + i < x.size(); i++) {
        x[start + i] += cf32(amp * (float)std::cos(ph), amp * (float)std::sin(ph));
        ph += 2.0 * M_PI * (offsetHz + devHz * freq[i]) / fs;
    }
}

inline std::vector<uint8_t> toBits(const std::vector<uint8_t>& b, bool msb = true) {
    std::vector<uint8_t> o;
    for (uint8_t v : b) for (int i = 0; i < 8; i++) o.push_back(msb ? (v >> (7 - i)) & 1 : (v >> i) & 1);
    return o;
}

inline void addNoise(std::vector<cf32>& x, double sigma, unsigned seed = 7) {
    std::mt19937 g(seed);
    std::normal_distribution<float> n(0.f, (float)sigma);
    for (auto& v : x) v += cf32(n(g), n(g));
}

// LoRa frame at carrier offsetHz, startSec after the first sample
inline void addLora(std::vector<cf32>& x, double fs, const dect2::lora::Params& p, const std::vector<uint8_t>& payload, double startSec, double offsetHz, float amp, bool invertIq = false) {
    dect2::lora::TxFrame f;
    f.p = p;
    f.data = dect2::lora::encode(p, payload.data(), payload.size());
    f.startSec = startSec;
    f.freqHz = invertIq ? -offsetHz : offsetHz;
    f.amp = amp;
    std::vector<cf32> tmp(x.size(), cf32(0, 0));
    f.render(tmp.data(), tmp.size(), 0.0, fs);
    for (size_t i = 0; i < x.size(); i++) x[i] += invertIq ? std::conj(tmp[i]) : tmp[i];
}
} // namespace rctest
