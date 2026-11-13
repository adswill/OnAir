// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Intermodulation finder: a receiver with a cubic nonlinearity makes products of two fading signals; the finder must list them as following the law, must call a real transmitter on
// the same frequency independent, must say "cannot tell" when the signals stay steady, and must find nothing in noise or with a single signal. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// two tones whose levels swing by +-swingDb with independent slow patterns, through y = x + k x |x|^2 (the receiver); extra: a steady independent tone at the product frequency
struct Scene {
    double fs, f1, f2, swing, k, extraHz, extraAmp, noise; bool withExtra; Rng rng{3}; double p1 = 0, p2 = 0, pe = 0;
    std::vector<cf32> operator()(double t0, size_t n) {
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        for (size_t i = 0; i < n; i++) {
            const double t = t0 + i / fs;
            const double a1 = 0.2 * std::pow(10.0, swing * std::sin(2 * kPi * 0.071 * t) / 20), a2 = 0.2 * std::pow(10.0, swing * std::sin(2 * kPi * 0.113 * t + 1.0) / 20);
            p1 += 2 * kPi * f1 / fs; p2 += 2 * kPi * f2 / fs; pe += 2 * kPi * extraHz / fs;
            cf32 v = (float)a1 * cf32((float)std::cos(p1), (float)std::sin(p1)) + (float)a2 * cf32((float)std::cos(p2), (float)std::sin(p2));
            const float pw = std::norm(v); v += (float)k * v * pw;
            if (withExtra) v += (float)extraAmp * cf32((float)std::cos(pe), (float)std::sin(pe));
            x[i] = v + cg(rng, sg, sg);
        }
        return x;
    }
};
static int countVerdict(const X40Telemetry& t, double mhz, const char* what) {
    int n = 0; for (auto& r : t.rows) if (r.kind == "Product" && std::fabs(atof(r.id.c_str()) - mhz) < 0.0004 && r.text.find(what) != std::string::npos) n++; return n;
}

int main() {
    const double fs = 1000000, centre = 145e6;
    const double f1 = 120000, f2 = 190000;      // products at 50 kHz and 260 kHz (and 70 kHz)
    const double noise = 2e-6;                    // the tones are about 50 dB over the noise per bin
    // 1. fading signals, k = 0.8: IM3 at 2f1-f2 = 50 kHz and 2f2-f1 = 260 kHz must follow the law, at three radio offsets of the whole scene
    for (double shift : {0.0, 777.0, -3333.0}) {
        Scene sc{fs, f1 + shift, f2 + shift, 5.0, 0.8, 0, 0, noise, false};
        auto t = runChunks(x40::Intermod, fs, 70.0, std::ref(sc), 0, centre, 0.5);
        const double m1 = (centre + 2 * (f1 + shift) - (f2 + shift)) / 1e6, m2 = (centre + 2 * (f2 + shift) - (f1 + shift)) / 1e6;
        CHECK(countVerdict(t, m1, "follows the intermod law") >= 1, "shift %.0f: 2f1-f2 (%.4f MHz) not reported as a product: %s", shift, m1, t.headline.c_str());
        CHECK(countVerdict(t, m2, "follows the intermod law") >= 1, "shift %.0f: 2f2-f1 (%.4f MHz) not reported", shift, m2);
        CHECK(countVerdict(t, m1, "independent") == 0 && countVerdict(t, m2, "independent") == 0, "shift %.0f: product called independent", shift);
    }
    // 2. no nonlinearity, but a real steady-level transmitter exactly at 2f1-f2: it must NOT be called a product
    { Scene sc{fs, f1, f2, 5.0, 0.0, 2 * f1 - f2, 0.004, noise, true};
      auto t = runChunks(x40::Intermod, fs, 70.0, std::ref(sc), 0, centre, 0.5);
      const double m1 = (centre + 2 * f1 - f2) / 1e6;
      CHECK(countVerdict(t, m1, "follows the intermod law") == 0, "a real transmitter was called a product");
      CHECK(countVerdict(t, m1, "independent") >= 1, "a real transmitter at 2f1-f2 not called independent (rows %zu)", t.rows.size()); }
    // 3. steady signals (no fading): the product is listed but the verdict is "cannot tell"
    { Scene sc{fs, f1, f2, 0.0, 0.8, 0, 0, noise, false};
      auto t = runChunks(x40::Intermod, fs, 40.0, std::ref(sc), 0, centre, 0.5);
      const double m1 = (centre + 2 * f1 - f2) / 1e6;
      CHECK(countVerdict(t, m1, "cannot tell") >= 1 && countVerdict(t, m1, "follows") == 0, "steady signals: %s", t.headline.c_str()); }
    // 4. product strength: k sweeps how far down the product is; the weakest found and the false products on a single signal and on noise
    printf("product below the weaker signal -> found as following the law\n");
    for (double k : {2.0, 0.8, 0.3, 0.1, 0.03}) {
        Scene sc{fs, f1, f2, 5.0, k, 0, 0, noise, false};
        auto t = runChunks(x40::Intermod, fs, 70.0, std::ref(sc), 0, centre, 0.5);
        const double m1 = (centre + 2 * f1 - f2) / 1e6; const bool ok = countVerdict(t, m1, "follows") >= 1;
        double below = 0; for (auto& r : t.rows) if (r.kind == "Product" && std::fabs(atof(r.id.c_str()) - m1) < 0.0004) sscanf(strstr(r.text.c_str(), "MHz,") ? strstr(r.text.c_str(), "MHz,") + 5 : "0", "%lf", &below);
        printf("  k %.2f: %.0f dB below, %s\n", k, below, ok ? "found" : "not found");
        if (k >= 0.3) CHECK(ok, "k %.2f: not found", k);
    }
    { Scene sc{fs, f1, f2, 5.0, 0.0, 0, 0, noise, false}; sc.f2 = 0; sc.f2 = f1 + 1;   // one signal (two tones 1 Hz apart collapse into one)
      auto t = runChunks(x40::Intermod, fs, 60.0, std::ref(sc), 0, centre, 0.5); CHECK(countKind(t, "Product") == 0, "single signal: %d products", countKind(t, "Product")); }
    { Scene sc{fs, f1, f2, 5.0, 0.0, 0, 0, 1.0, false}; sc.k = 0; auto t = runChunks(x40::Intermod, fs, 40.0, std::ref(sc), 0, centre, 0.5);
      // noise power 1.0 swamps the 0.2-amplitude tones: nothing to find
      CHECK(countKind(t, "Product") == 0, "noise: %d products", countKind(t, "Product")); }
    printf(fails ? "x40 intermod: %d FAILED\n" : "x40 intermod: ok\n", fails);
    return fails ? 1 : 0;
}
