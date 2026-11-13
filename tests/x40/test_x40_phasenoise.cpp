// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Phase noise meter: a carrier with white-FM phase noise (Lorentzian, linewidth dnu: L(f) = dnu / (2 pi f^2)) and a white phase floor, with a discrete phase-modulation spur, read against the theory
// at 100 Hz to 100 kHz, at three sample rates, with a carrier offset and drift, and additive noise. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Osc {
    double fs, dnu, floorDb, hz, drift, noise, spurHz = 0, spurDbc = -200; Rng rng{9}; double phi = 0, ph = 0, ps = 0;
    std::vector<cf32> operator()(double t0, size_t n) {
        std::vector<cf32> x(n); const double sw = std::sqrt(2 * kPi * dnu / fs), sfl = std::sqrt(2.0 * std::pow(10.0, floorDb / 10) * 2 * fs / 2 / 2);    // white phase: S_phi (rad^2/Hz one-sided) = 2 L
        const float sg = (float)std::sqrt(noise / 2); const double spurAmp = 2 * std::pow(10.0, spurDbc / 20);        // phase modulation index m for a spur of level m/2 (in amplitude)
        for (size_t i = 0; i < n; i++) {
            const double t = t0 + i / fs; phi += sw * rng.g(); ps += 2 * kPi * spurHz / fs;
            const double total = phi + sfl * rng.g() + spurAmp * std::sin(ps);
            ph += 2 * kPi * (hz + drift * t) / fs;
            const double a = ph + total;
            x[i] = cf32((float)std::cos(a), (float)std::sin(a)) + cg(rng, sg, sg);
        }
        return x;
    }
};
static double L(const X40Telemetry& t, const char* at) { for (auto& f : t.facts) if (f.first == std::string("phase noise at ") + at) return atof(f.second.c_str()); return -999; }
static double theory(double dnu, double floorDb, double f) { return 10 * std::log10(dnu / (2 * kPi * f * f) + std::pow(10.0, floorDb / 10)); }

int main() {
    // 1. linewidth 20 Hz, white phase floor -125 dBc/Hz; carrier offset and drift; three input rates
    for (double fs : {250000.0, 500000.0, 1000000.0}) {
        const double off = fs == 250000 ? 12345.6 : fs == 500000 ? -48000.0 : 3000.0;
        Osc o{fs, 20.0, -125.0, off, 2.0, 1e-9}; 
        auto t = runChunks(x40::PhaseNoise, fs, 24.0, std::ref(o), 0, 10e6, 1.0);
        for (auto [name, f] : {std::pair<const char*, double>{"100 Hz", 100}, {"1 kHz", 1000}, {"10 kHz", 10000}, {"100 kHz", 100000}}) {
            const double got = L(t, name), want = theory(20.0, -125.0, f);
            CHECK(std::fabs(got - want) < 1.5, "fs %.0f %s: %.1f dBc/Hz want %.1f (%s)", fs, name, got, want, t.headline.c_str());
        }
        if (fs == 250000) printf("  fs 250000: L 100 Hz / 1 / 10 / 100 kHz read %.1f / %.1f / %.1f / %.1f, theory %.1f / %.1f / %.1f / %.1f\n", L(t, "100 Hz"), L(t, "1 kHz"), L(t, "10 kHz"), L(t, "100 kHz"), theory(20, -125, 100), theory(20, -125, 1e3), theory(20, -125, 1e4), theory(20, -125, 1e5));
    }
    // 2. a spur of -62 dBc at 1.5 kHz is listed with its level; the jitter against the integral of the theory
    { Osc o{250000, 0.2, -130.0, 500, 0, 1e-9, 1500, -62}; auto t = runChunks(x40::PhaseNoise, 250000, 24.0, std::ref(o), 0, 10e6, 1.0);
      bool found = false; for (auto& f : t.facts) if (f.first.rfind("spur", 0) == 0) { double hz, dbc; if (sscanf(f.second.c_str(), "%lf Hz from the carrier, %lf dBc", &hz, &dbc) == 2 && std::fabs(std::fabs(hz) - 1500) < 10 && std::fabs(dbc + 62) < 2.0) found = true; }
      CHECK(found, "the -62 dBc spur at 1.5 kHz was not listed"); }
    { Osc o{250000, 20.0, -125.0, 500, 0, 1e-9}; auto t = runChunks(x40::PhaseNoise, 250000, 24.0, std::ref(o), 0, 10e6, 1.0);
      // sigma^2 = 2 * integral of L over 100 Hz - 100 kHz: for dnu/(2 pi f^2) the integral is dnu/(2 pi) (1/100 - 1/1e5), plus the floor times the bandwidth
      const double want = std::sqrt(2 * (20.0 / (2 * kPi) * (1.0 / 100 - 1.0 / 1e5) + std::pow(10.0, -12.5) * (1e5 - 100)));
      double got = -1; for (auto& f : t.facts) if (f.first.rfind("integrated phase jitter", 0) == 0) got = atof(f.second.c_str()) / 1000;
      CHECK(std::fabs(got - want) < 0.15 * want, "jitter %.5f rad want %.5f", got, want); }
    // 3. additive noise sets the floor: carrier to noise in 200 kHz of 50, 40, 30 dB with a quiet oscillator: L(100 kHz) reads the noise (-10log(200e3) - CNR - ...)
    printf("additive noise: carrier to noise in 200 kHz -> L(100 kHz) read, expected noise floor (dBc/Hz):");
    for (double cnr : {70.0, 60.0, 50.0, 40.0}) {
        Osc o{250000, 0.01, -150.0, 500, 0, noiseFor(cnr, 200000, 250000)}; auto t = runChunks(x40::PhaseNoise, 250000, 24.0, std::ref(o), 0, 10e6, 1.0);
        const double want = 10 * std::log10(std::pow(10.0, -cnr / 10) / 200000.0 + 0.01 / (2 * kPi * 1e10) + std::pow(10.0, -15.0)); const double got = L(t, "100 kHz");
        printf(" %.0f: %.1f (%.1f)", cnr, got, want);
        CHECK(std::fabs(got - want) < 1.5, "cnr %.0f: %.1f want %.1f", cnr, got, want);
    }
    printf("\n");
    // 4. no carrier: nothing
    { ToneScene sc(250000, 1.0, 4); auto t = runChunks(x40::PhaseNoise, 250000, 24.0, std::ref(sc), 0, 10e6, 1.0); CHECK(t.rows.empty(), "noise: %zu rows", t.rows.size()); }
    printf(fails ? "x40 phasenoise: %d FAILED\n" : "x40 phasenoise: ok\n", fails);
    return fails ? 1 : 0;
}
