// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/kaal/kaal_phy.h"
#include <cmath>

namespace dect2 {
namespace kaal {

static const double kPi = 3.14159265358979323846;

cf pilotSymbol() { const float a = 3.f / std::sqrt(10.f); return cf(a, a); }

std::vector<cf> preamble() {
    std::vector<cf> p(kPreambleLen);
    const double r = 1.0 / std::sqrt(2.0);
    for (int i = 0; i < kPreambleLen; i++) {
        const double re = ((i * 7 + 3) % 5 < 2) ? 1.0 : -1.0;
        const double im = ((i * 11 + 1) % 3) ? 1.0 : -1.0;
        p[i] = cf((float)(re * r), (float)(im * r));
    }
    return p;
}

static int grayLevel(int b0, int b1) { return b0 == 0 ? (b1 == 0 ? -3 : -1) : (b1 == 1 ? 1 : 3); }

std::vector<cf> qam16Map(const uint8_t* bits, size_t nbits) {
    const size_t n = nbits / 4;
    const float s = 1.f / std::sqrt(10.f);
    std::vector<cf> out(n);
    for (size_t i = 0; i < n; i++) {
        const uint8_t* b = bits + 4 * i;
        out[i] = cf(grayLevel(b[0] & 1, b[1] & 1) * s, grayLevel(b[2] & 1, b[3] & 1) * s);
    }
    return out;
}

std::vector<float> qam16DemapSoft(const cf* sym, size_t n, float noiseVar) {
    static const int lv[4] = {-3, -1, 1, 3};
    static const int bitOf[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};   // bits of each level
    const float k = std::sqrt(10.f), den = noiseVar * 10.f + 1e-12f;
    std::vector<float> out(4 * n);
    for (size_t i = 0; i < n; i++) {
        const float c[2] = {sym[i].real() * k, sym[i].imag() * k};
        for (int comp = 0; comp < 2; comp++)
            for (int bp = 0; bp < 2; bp++) {
                float m0 = 1e30f, m1 = 1e30f;
                for (int l = 0; l < 4; l++) {
                    const float d = (c[comp] - lv[l]) * (c[comp] - lv[l]);
                    if (bitOf[l][bp] == 0) { if (d < m0) m0 = d; } else if (d < m1) m1 = d;
                }
                out[4 * i + 2 * comp + bp] = (m1 - m0) / den;
            }
    }
    return out;
}

uint64_t xorshift64(uint64_t& x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

std::vector<uint32_t> interleavePerm(size_t n) {
    std::vector<uint32_t> p(n);
    for (size_t i = 0; i < n; i++) p[i] = (uint32_t)i;
    uint64_t st = kInterleaveSeed;
    for (size_t i = n; i-- > 1;) {
        const size_t j = (size_t)(xorshift64(st) % (i + 1));
        const uint32_t t = p[i]; p[i] = p[j]; p[j] = t;
    }
    return p;
}

std::vector<uint32_t> interleavePermInv(size_t n) {
    const std::vector<uint32_t> p = interleavePerm(n);
    std::vector<uint32_t> inv(n);
    for (size_t i = 0; i < n; i++) inv[p[i]] = (uint32_t)i;
    return inv;
}

std::vector<uint8_t> interleave(const std::vector<uint8_t>& in) {
    const std::vector<uint32_t> p = interleavePerm(in.size());
    std::vector<uint8_t> out(in.size());
    for (size_t i = 0; i < in.size(); i++) out[i] = in[p[i]];
    return out;
}

std::vector<float> deinterleave(const std::vector<float>& in) {
    const std::vector<uint32_t> inv = interleavePermInv(in.size());
    std::vector<float> out(in.size());
    for (size_t i = 0; i < in.size(); i++) out[i] = in[inv[i]];
    return out;
}

std::vector<double> rrcTaps() {
    const double beta = kBeta;
    const int half = kRrcSpan * kSps / 2;
    std::vector<double> h(2 * half + 1);
    double e = 0;
    for (int n = -half; n <= half; n++) {
        const double t = (double)n / kSps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1 - beta + 4 * beta / kPi;
        else if (std::fabs(std::fabs(t) - 1 / (4 * beta)) < 1e-9)
            v = beta / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * beta)) + (1 - 2 / kPi) * std::cos(kPi / (4 * beta)));
        else v = (std::sin(kPi * t * (1 - beta)) + 4 * beta * t * std::cos(kPi * t * (1 + beta))) / (kPi * t * (1 - (4 * beta * t) * (4 * beta * t)));
        h[n + half] = v;
        e += v * v;
    }
    const double s = 1.0 / std::sqrt(e);
    for (double& v : h) v *= s;
    return h;
}

std::vector<cf> sweepTemplate(size_t nSamples, double t0) {
    std::vector<cf> out(nSamples);
    double acc = 0;   // cumulative sum of instantaneous frequency (Hz); phase = 2 pi acc / Fs
    for (size_t i = 0; i < nSamples; i++) {
        const double t = t0 + (double)i / kFs;
        const double ph = std::fmod(t, kSweepPeriodS);
        acc += kSweepLowHz + (kSweepHighHz - kSweepLowHz) * (ph / kSweepPeriodS);
        const double a = 2 * kPi * acc / kFs;
        out[i] = cf((float)std::cos(a), (float)std::sin(a));
    }
    return out;
}

}  // namespace kaal
}  // namespace dect2
