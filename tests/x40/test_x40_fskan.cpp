// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FSK parameter analyser: 2-FSK, GFSK, MSK and 4-FSK signals with known symbol rate, shift and levels (random data), carrier offsets, clock error and noise; the rate, the level count, the shift, the
// modulation index and the centre are compared with the generator; a noise sweep; false reports on noise, a plain carrier and an FM voice-like signal. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// levels: +-1 (2-level) or +-1, +-3 (4-level) times dev; Gaussian pulse shaping with BT (0 = none, a 1/8-symbol smoothing only)
static std::vector<cf32> fsk(int levels, double baud, double dev, double bt, double fs, double seconds, double off, double ppm, unsigned seed, double noise) {
    std::mt19937 g(seed); const long nsym = (long)(seconds * baud * (1 + ppm * 1e-6)) + 40; std::vector<double> a((size_t)nsym);
    for (auto& v : a) { if (levels == 2) v = (g() & 1) ? 1.0 : -1.0; else { const int q = (int)(g() % 4); v = (q - 1.5) * 2.0 / 3.0 * 1.0; v = (q == 0 ? -3 : q == 1 ? -1 : q == 2 ? 1 : 3) / (levels == 4 ? 3.0 : 1.0); } }
    const double T = 1.0 / baud / (1 + ppm * 1e-6); const size_t N = (size_t)(fs * seconds); std::vector<cf32> x(N);
    const double sigma = (bt > 0 ? std::sqrt(std::log(2.0)) / (2 * kPi * bt) : 0.12) * T;           // Gaussian pulse sigma in seconds
    double ph = 0; Rng r(seed + 1); const float sg = (float)std::sqrt(noise / 2);
    for (size_t i = 0; i < N; i++) {
        const double t = i / fs; const long k0 = (long)std::floor(t / T); double f = 0, wsum = 0;
        for (long k = k0 - 3; k <= k0 + 3; k++) { if (k < 0 || k >= nsym) continue; const double tc = (k + 0.5) * T; const double w = 0.5 * (std::erf((t - tc + T / 2) / (std::sqrt(2.0) * sigma)) - std::erf((t - tc - T / 2) / (std::sqrt(2.0) * sigma))); f += a[(size_t)k] * w; wsum += w; }
        ph += 2 * kPi * (off + dev * f) / fs;
        x[i] = cf32((float)std::cos(ph), (float)std::sin(ph)) + cg(r, sg, sg);
    }
    return x;
}
static double fnum(const X40Telemetry& t, const char* key) { return factNum(t, key); }

int main() {
    const double fs = 250000;
    struct Case { int levels; double baud, dev, bt, off, ppm; const char* name; };
    for (Case c : {Case{2, 1200, 600, 0, 0, 0, "1200 Bd FSK +-600"}, Case{2, 9600, 2400, 0.5, 5000, 150, "9600 Bd GFSK BT 0.5 +-2.4k"}, Case{2, 4800, 1200, 0, -7300, 0, "4800 Bd MSK"}, Case{2, 19200, 9600, 0.5, 2000, -200, "19.2 kBd GFSK +-9.6k"}, Case{4, 4800, 1800, 0, 1000, 0, "4800 Bd 4-FSK +-0.6/1.8k"}, Case{4, 2400, 900, 0.5, -3000, 100, "2400 Bd 4-GFSK"}, Case{2, 600, 300, 0, 1500, 50, "600 Bd FSK +-300"}}) {
        auto x = fsk(c.levels, c.baud, c.dev, c.bt, fs, 12.0, c.off, c.ppm, 3, noiseFor(30, 50000, fs));
        auto t = run(x40::FskAnalyser, x, fs);
        const double baud = fnum(t, "symbol rate"), shift = fnum(t, "shift"), centre = fnum(t, "centre frequency"), h = factNum(t, "modulation index") < -900 ? 0 : atof(strstr(factStr(t, "modulation index").c_str(), "= ") + 2);
        const double wantShift = c.levels == 2 ? 2 * c.dev : 2 * c.dev / 3.0, wantH = (c.levels == 2 ? 2 * c.dev : 2 * c.dev) / c.baud;
        printf("  %s: read %s\n", c.name, t.headline.c_str());
        CHECK(factStr(t, "modulation").rfind(c.levels == 2 ? "2-level" : "4-level", 0) == 0, "%s: modulation %s", c.name, factStr(t, "modulation").c_str());
        CHECK(std::fabs(baud - c.baud * (1 + c.ppm * 1e-6)) < 0.004 * c.baud + 1, "%s: baud %.1f want %.1f", c.name, baud, c.baud * (1 + c.ppm * 1e-6));
        CHECK(std::fabs(shift - wantShift * (c.bt > 0 ? 0.93 : 1.0)) < 0.05 * wantShift, "%s: shift %.0f want %.0f", c.name, shift, wantShift);
        CHECK(std::fabs(centre - c.off) < 40, "%s: centre %.0f want %.0f", c.name, centre, c.off);
        CHECK(std::fabs(h - wantH * (c.bt > 0 ? 0.93 : 1.0)) < 0.06 * wantH + 0.02, "%s: h %.2f want %.2f", c.name, h, wantH);
        CHECK(factStr(t, "modulation").find(std::string(c.name).find("MSK") != std::string::npos ? "MSK-like" : "level") != std::string::npos, "%s: MSK note", c.name);
    }
    // noise sweep, 9600 Bd GFSK +-2.4k: carrier to noise in 50 kHz
    printf("noise sweep (carrier to noise in 50 kHz): 9600 Bd GFSK read as (levels, baud, shift)\n");
    for (double cnr : {30.0, 22.0, 18.0, 15.0, 12.0}) {
        auto x = fsk(2, 9600, 2400, 0.5, fs, 12.0, 1500, 0, 5, noiseFor(cnr, 50000, fs)); auto t = run(x40::FskAnalyser, x, fs);
        const std::string m = factStr(t, "modulation"); printf("  %.0f dB: %s baud %.0f shift %.0f\n", cnr, m.empty() ? "none" : m.substr(0, 7).c_str(), fnum(t, "symbol rate"), fnum(t, "shift"));
        if (cnr >= 18) CHECK(m.rfind("2-level", 0) == 0 && std::fabs(fnum(t, "shift") - 4800 * 0.93) < 0.08 * 4800 && std::fabs(fnum(t, "symbol rate") - 9600) < 40, "%.0f dB: %s", cnr, m.c_str());
    }
    // false reports: noise, a plain carrier, FM voice-like (two audio tones with +-3 kHz deviation, no symbol grid)
    { int falseRows = 0;
      std::vector<cf32> n((size_t)(fs * 20), cf32(0, 0)); addNoise(n, 1.0, 2); falseRows += (int)run(x40::FskAnalyser, n, fs).rows.size();
      std::vector<cf32> c((size_t)(fs * 20), cf32(1, 0)); mix(c, 3000, 0, fs); addNoise(c, 0.01, 3); falseRows += (int)run(x40::FskAnalyser, c, fs).rows.size();
      std::vector<cf32> f((size_t)(fs * 20)); { double p = 0; for (size_t i = 0; i < f.size(); i++) { const double tt = i / fs; p += 2 * kPi * (2000 + 3000 * std::sin(2 * kPi * 440 * tt) + 1500 * std::sin(2 * kPi * 1130 * tt)) / fs; f[i] = cf32((float)std::cos(p), (float)std::sin(p)); } } addNoise(f, 0.01, 4); falseRows += (int)run(x40::FskAnalyser, f, fs).rows.size();
      printf("noise, carrier, FM voice-like, 3 x 20 s: %d false reports\n", falseRows); CHECK(falseRows == 0, "false reports %d", falseRows); }
    printf(fails ? "x40 fskan: %d FAILED\n" : "x40 fskan: ok\n", fails);
    return fails ? 1 : 0;
}
