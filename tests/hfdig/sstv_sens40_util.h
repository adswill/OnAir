// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared by the SSTV 4.0 sensitivity bench and guard test: a textured test picture, a frequency shifter and a clock error that work on
// any picture's audio, white noise in 3 kHz (as test_hfdig_sens39d.cpp), and the picture's error against the sent one. Simulated only.
#pragma once
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace sstvbench {

using namespace dect2;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 3) {}
    double u() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return ((s >> 11) + 0.5) / 9007199254740992.0; }
    double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); }
};

inline int modeIndex(const char* name) {
    for (int m = 0; m < sstvModeCount(); m++) if (std::string(sstvModeInfo(m).name) == name) return m;
    return -1;
}

// A picture with detail everywhere: smooth colour fields, sharp-edged shapes, fine stripes (4 and 8 pixel periods) and small text.
inline void texturedPicture(int w, int h, std::vector<uint8_t>& rgb) {
    rgb.assign((size_t)w * h * 3, 0);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const double u = (double)x / w, v = (double)y / h;
        double r = 128 + 90 * std::sin(6.2832 * (1.3 * u + 0.4 * v)), g = 128 + 90 * std::sin(6.2832 * (0.7 * u - 1.1 * v) + 1.0),
               b = 128 + 90 * std::cos(6.2832 * (2.1 * u * v + 0.3 * u));
        const double cx = u - 0.3, cy = v - 0.35;
        if (cx * cx + cy * cy < 0.02) { r = 230; g = 40; b = 40; }
        if (u > 0.55 && u < 0.85 && v > 0.15 && v < 0.4) { r = 20; g = 200; b = 60; }
        if (v > 0.55 && v < 0.7) { const int p = u < 0.5 ? 8 : 4; const bool on = (x / (p / 2)) & 1; r = g = b = on ? 235 : 20; }
        if (v > 0.78 && v < 0.9 && ((x / 3 + y / 3) % 7 == 0)) { r = g = b = 250; }
        uint8_t* o = &rgb[((size_t)y * w + x) * 3];
        o[0] = (uint8_t)std::clamp(r, 0.0, 255.0); o[1] = (uint8_t)std::clamp(g, 0.0, 255.0); o[2] = (uint8_t)std::clamp(b, 0.0, 255.0);
    }
}

// Real audio shifted by offHz (analytic signal from a 255-tap Hilbert filter) and resampled for a clock error of ppm.
inline std::vector<float> impair(const std::vector<float>& in, double offHz, double ppm) {
    std::vector<float> x = in;
    if (offHz != 0) {
        const int N = 255, D = N / 2;
        std::vector<double> h((size_t)N, 0.0);
        for (int k = 0; k < N; k++) {
            const int n = k - D;
            const double w = 0.42 - 0.5 * std::cos(2 * dect2::math::kPi * k / (N - 1)) + 0.08 * std::cos(4 * dect2::math::kPi * k / (N - 1));
            h[(size_t)k] = (n % 2) ? 2.0 / (dect2::math::kPi * n) * w : 0.0;
        }
        std::vector<float> y(x.size(), 0.f);
        for (size_t i = 0; i < x.size(); i++) {
            double q = 0;
            for (int k = 0; k < N; k++) { const long j = (long)i - k; if (j >= 0) q += h[(size_t)k] * x[(size_t)j]; }
            const double re = i >= (size_t)D ? x[i - D] : 0.0;
            const double ph = 2 * dect2::math::kPi * offHz * (double)i / 8000.0;
            y[i] = (float)(re * std::cos(ph) - q * std::sin(ph));
        }
        x.swap(y);
    }
    if (ppm != 0) {   // the receiver's clock is fast by ppm: it takes 1 + ppm samples where the sender sent 1 (cubic Hermite)
        const double r = 1.0 / (1.0 + ppm * 1e-6);
        std::vector<float> y;
        y.reserve(x.size());
        for (double t = 1; t < (double)x.size() - 3; t += r) {
            const size_t i = (size_t)t; const double f = t - (double)i;
            const double p0 = x[i - 1], p1 = x[i], p2 = x[i + 1], p3 = x[i + 2];
            const double c1 = 0.5 * (p2 - p0), c2 = p0 - 2.5 * p1 + 2 * p2 - 0.5 * p3, c3 = 0.5 * (p3 - p0) + 1.5 * (p1 - p2);
            y.push_back((float)(((c3 * f + c2) * f + c1) * f + p1));
        }
        x.swap(y);
    }
    return x;
}

inline double sigmaFor(double snrDb) { return std::sqrt(0.125 / std::pow(10.0, snrDb / 10.0) * 4000.0 / 3000.0); }   // a 0.5 tone, noise in 3 kHz

struct Result { bool complete = false; double mae = 255, psnr = 0; int lines = 0; };

inline Result score(const HfdigSstvTelemetry& t, const std::vector<uint8_t>& ref) {
    Result r;
    if (t.picturesDone < 1 || !t.image || t.image->rgb.size() != ref.size() || ref.empty()) return r;
    r.complete = t.image->complete; r.lines = t.image->lines;
    const std::vector<uint8_t>& im = t.image->rgb;
    double s = 0, s2 = 0;
    for (size_t i = 0; i < ref.size(); i++) { const double d = (int)im[i] - (int)ref[i]; s += std::fabs(d); s2 += d * d; }
    r.mae = s / (double)ref.size();
    r.psnr = 10 * std::log10(255.0 * 255.0 / std::max(1e-9, s2 / (double)ref.size()));
    return r;
}

// One picture: pic 0 the test picture, 1 the textured one.
inline Result trial(int m, int pic, double snr, uint64_t seed, double offHz, double ppm, bool filter = true) {
    const SstvModeInfo& mi = sstvModeInfo(m);
    std::vector<uint8_t> ref;
    if (pic == 1) texturedPicture(mi.width, mi.height, ref); else sstvTestPicture(mi.width, mi.height, ref);
    auto gen = makeSstvTx(m, ref);
    const double sec = 0.4 + 0.91 + mi.lineMs * (mi.width > 320 && mi.height > 300 ? mi.height / 2 : mi.height) / 1000.0 + 4;
    std::vector<float> a((size_t)(sec * 8000));
    gen->generate(a.data(), a.size());
    a = impair(a, offHz, ppm);
    auto dec = makeSstvDecoder();
    dec->setNoiseFilter(filter);   // off: the 3.9 picture
    Rng nz(seed);
    const double sg = sigmaFor(snr);
    HfdigSstvTelemetry t;
    for (size_t o = 0; o < a.size(); o += 800) {
        const size_t n = std::min<size_t>(800, a.size() - o);
        for (size_t i = 0; i < n; i++) a[o + i] += (float)(sg * nz.g());
        dec->feedAudio(a.data() + o, n);
        if ((o / 800) % 10 == 0) { dec->telemetry(t); if (t.picturesDone >= 1) break; }
    }
    dec->telemetry(t);
    return score(t, ref);
}

// Noise alone (level of a 0 dB signal's noise in 3 kHz) for secs: the pictures the decoder reports (started by a false VIS or false pulses).
inline uint64_t noiseOnlyPictures(double secs, uint64_t seed) {
    auto dec = makeSstvDecoder();
    Rng nz(seed);
    std::vector<float> b(800);
    const double sg = sigmaFor(0);
    for (size_t k = 0; k < (size_t)(secs * 10); k++) {
        for (float& v : b) v = (float)(sg * nz.g());
        dec->feedAudio(b.data(), b.size());
    }
    HfdigSstvTelemetry t;
    dec->telemetry(t);
    return t.picturesDone;
}

} // namespace sstvbench
