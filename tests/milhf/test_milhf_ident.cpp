// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF military waveform identifier: synthetic signals at 10 dB SNR must be ranked first, noise must give no signal.
#include "dect2/milhf/milhf_rx.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using cd = std::complex<double>;
static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { std::printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

static const double kFs = 12000.0, kPi = 3.14159265358979323846;

static std::vector<double> rrc(double sps, double beta, int span) {
    const int L = (int)(span * sps) | 1;
    std::vector<double> h(L);
    for (int i = 0; i < L; i++) {
        const double t = (i - L / 2) / sps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1.0 - beta + 4 * beta / kPi;
        else if (std::fabs(std::fabs(4 * beta * t) - 1) < 1e-6) v = beta / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * beta)) + (1 - 2 / kPi) * std::cos(kPi / (4 * beta)));
        else v = (std::sin(kPi * t * (1 - beta)) + 4 * beta * t * std::cos(kPi * t * (1 + beta))) / (kPi * t * (1 - std::pow(4 * beta * t, 2)));
        h[i] = v;
    }
    return h;
}

// add white noise so that the signal power over the noise power in 3 kHz is snrDb
static void addNoise(std::vector<float>& x, double snrDb, std::mt19937& g) {
    double p = 0;
    for (float v : x) p += (double)v * v;
    p /= (double)x.size();
    const double sig = std::sqrt(2.0 * p / std::pow(10.0, snrDb / 10.0));   // noise power over 6 kHz is twice that over 3 kHz
    std::normal_distribution<double> nd(0, sig);
    for (auto& v : x) v += (float)nd(g);
}

// single-carrier 2400 Bd PSK at fc: frame = known part (nKnown symbols from a fixed sequence) + random data, M-PSK
static std::vector<float> serialTone(std::mt19937& g, double fc, int M, int frame, int nKnown, double seconds, double baud = 2400.0) {
    const double sps = kFs / baud;
    const size_t nSym = (size_t)(seconds * baud);
    std::mt19937 gk(12345);
    std::vector<int> known(nKnown);
    for (auto& k : known) k = (int)(gk() % M);
    std::vector<cd> s(nSym);
    for (size_t i = 0; i < nSym; i++) {
        const int pos = (int)(i % frame);
        const int sym = pos < nKnown ? known[pos] : (int)(g() % M);
        s[i] = std::polar(1.0, 2 * kPi * sym / M + kPi / M);
    }
    const std::vector<double> h = rrc(sps, 0.35, 8);
    const size_t n = (size_t)(nSym * sps);
    std::vector<float> x(n);
    for (size_t i = 0; i < n; i++) {
        cd acc(0, 0);
        const long c = (long)std::floor(i / sps);
        for (long k = c - 5; k <= c + 5; k++) {
            if (k < 0 || k >= (long)nSym) continue;
            const long idx = (long)i - (long)std::lround(k * sps) + (long)h.size() / 2;
            if (idx >= 0 && idx < (long)h.size()) acc += s[k] * h[idx];
        }
        x[i] = (float)(acc * std::polar(1.0, 2 * kPi * fc * i / kFs)).real();
    }
    return x;
}

// Link 11 like: 605 Hz Doppler tone, 2915 Hz sync tone, 14 data tones 110 Hz apart, DPSK at 75 Bd
static std::vector<float> link11(std::mt19937& g, double seconds) {
    const size_t n = (size_t)(seconds * kFs);
    std::vector<float> x(n, 0.f);
    const int symLen = (int)(kFs / 75.0);
    std::vector<double> f;
    for (int t = 0; t < 14; t++) f.push_back(935.0 + 110.0 * t);
    std::vector<double> ph(f.size(), 0.0);
    for (size_t i = 0; i < n; i++) {
        if (i % symLen == 0) for (auto& p : ph) p += (g() % 4) * kPi / 2;
        double v = 0;
        for (size_t t = 0; t < f.size(); t++) v += std::sin(2 * kPi * f[t] * i / kFs + ph[t]);
        v += 2.0 * std::sin(2 * kPi * 605.0 * i / kFs) + 1.0 * std::sin(2 * kPi * 2915.0 * i / kFs);
        x[i] = (float)v;
    }
    return x;
}

// MFSK: one of nTones tones per symbol, phase continuous
static std::vector<float> mfsk(std::mt19937& g, int nTones, double f0, double spacing, double baud, double seconds) {
    const size_t n = (size_t)(seconds * kFs);
    std::vector<float> x(n);
    const int symLen = (int)(kFs / baud);
    double ph = 0, f = f0;
    for (size_t i = 0; i < n; i++) {
        if (i % symLen == 0) f = f0 + spacing * (double)(g() % nTones);
        ph += 2 * kPi * f / kFs;
        x[i] = (float)std::sin(ph);
    }
    return x;
}

static std::string top(const dect2::milhf::Result& r) { return r.cands.empty() ? "" : r.cands[0].name; }
static bool has(const dect2::milhf::Result& r, const char* s) { for (auto& c : r.cands) if (c.name.find(s) != std::string::npos) return true; return false; }

int main() {
    const int trials = 20;
    int ok4285 = 0, ok110 = 0, okL11 = 0, okMf = 0, okFam = 0, okOff = 0;
    for (int t = 0; t < trials; t++) {
        std::mt19937 g(1000 + t);
        auto a = serialTone(g, 1800, 8, 256, 80, 6.0);
        addNoise(a, 10, g);
        auto r = dect2::milhf::analyze(a.data(), a.size(), kFs);
        if (top(r) == "STANAG 4285") ok4285++;
        if (has(r, "2400 Bd serial-tone")) okFam++;
        if (t == 0) std::printf("4285: top=%s conf=%.2f baud=%.1f order=%d lag=%d bw=%.0f centre=%.0f\n", top(r).c_str(), r.cands.empty() ? 0 : r.cands[0].conf, r.f.baudHz, r.f.order, r.f.frameLagSym, r.f.bwHz, r.f.centreHz);

        auto b = serialTone(g, 1800, 8, 48, 16, 6.0);
        addNoise(b, 10, g);
        r = dect2::milhf::analyze(b.data(), b.size(), kFs);
        if (top(r).find("188-110A") != std::string::npos) ok110++;
        if (t == 0) std::printf("110A: top=%s baud=%.1f order=%d lag=%d\n", top(r).c_str(), r.f.baudHz, r.f.order, r.f.frameLagSym);

        auto c = serialTone(g, 1830, 8, 256, 80, 6.0);
        addNoise(c, 10, g);
        r = dect2::milhf::analyze(c.data(), c.size(), kFs);
        if (top(r) == "STANAG 4285") okOff++;

        auto l = link11(g, 6.0);
        addNoise(l, 10, g);
        r = dect2::milhf::analyze(l.data(), l.size(), kFs);
        if (top(r).find("Link 11") != std::string::npos) okL11++;
        if (t == 0) std::printf("L11: top=%s comb=%.2f spacing=%.1f tones=%d pilot=%d\n", top(r).c_str(), r.f.combScore, r.f.toneSpacingHz, r.f.nTones, (int)r.f.pilot605);

        auto m = mfsk(g, 12, 800, 100, 50, 6.0);
        addNoise(m, 10, g);
        r = dect2::milhf::analyze(m.data(), m.size(), kFs);
        if (top(r).find("Multi-tone MFSK") != std::string::npos) okMf++;
        if (t == 0) std::printf("MFSK: top=%s comb=%.2f spacing=%.1f tones=%d\n", top(r).c_str(), r.f.combScore, r.f.toneSpacingHz, r.f.nTones);
    }
    std::printf("rates /%d: 4285 %d, family %d, 110A %d, offset 4285 %d, Link11 %d, MFSK %d\n", trials, ok4285, okFam, ok110, okOff, okL11, okMf);
    CHECK(ok4285 >= 16, "STANAG 4285-like identified %d/%d", ok4285, trials);
    CHECK(okFam >= 18, "serial-tone family found %d/%d", okFam, trials);
    CHECK(ok110 >= 16, "110A-like identified %d/%d", ok110, trials);
    CHECK(okOff >= 16, "4285 with an offset %d/%d", okOff, trials);
    CHECK(okL11 >= 16, "Link 11-like identified %d/%d", okL11, trials);
    CHECK(okMf >= 16, "12-tone MFSK identified %d/%d", okMf, trials);

    // noise only: no signal, no candidate
    int fp = 0;
    for (int t = 0; t < 60; t++) {
        std::mt19937 g(5000 + t);
        std::normal_distribution<double> nd(0, 0.3);
        std::vector<float> x((size_t)(6 * kFs));
        for (auto& v : x) v = (float)nd(g);
        auto r = dect2::milhf::analyze(x.data(), x.size(), kFs);
        if (r.f.signal || !r.cands.empty()) fp++;
    }
    CHECK(fp == 0, "false positives on noise: %d/60", fp);

    std::printf("%s: %d failure(s)\n", fails ? "test_milhf_ident FAILED" : "test_milhf_ident OK", fails);
    return fails ? 1 : 0;
}
