// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Fading analyser: a carrier through a Rician / Rayleigh channel (sum of 48 Doppler-shifted paths, Jakes style) with the maximum Doppler of 5 and 50 Hz; the K-factor, the fade rate and length
// at -10 dB and the Doppler spread are compared with theory, with a carrier offset and drift, a noise sweep, the loss and return of the carrier and noise alone. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Chan {
    double fs, fd, K, lo, drift, noise; int N = 48; std::vector<cf32> ph, rot; Rng rng{7}; double ph0 = 0; bool init = false; double from = 0, to = 1e9;
    void setup() { Rng r(3); ph.resize(N); rot.resize(N); for (int n = 0; n < N; n++) { const double a = 2 * kPi * (n + r.g() * 0.0 + 0.37) / N + 0.7 * std::sin(1.3 * n), p = 2 * kPi * std::fabs(std::sin(7.9 * n + 1)); ph[n] = cf32((float)std::cos(p), (float)std::sin(p)); const double w = 2 * kPi * fd * std::cos(a) / fs; rot[n] = cf32((float)std::cos(w), (float)std::sin(w)); } init = true; }
    std::vector<cf32> operator()(double t0, size_t n) {
        if (!init) setup();
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        const float los = (float)std::sqrt(K / (K + 1)), sc = (float)std::sqrt(1.0 / (K + 1) / N);
        for (size_t i = 0; i < n; i++) {
            const double t = t0 + i / fs;
            cf32 s(0, 0); for (int k = 0; k < N; k++) { ph[k] *= rot[k]; s += ph[k]; }
            if ((i & 1023) == 0) for (int k = 0; k < N; k++) ph[k] /= std::abs(ph[k]);      // keep the rotators on the unit circle
            ph0 += 2 * kPi * (lo + drift * t) / fs; const cf32 car((float)std::cos(ph0), (float)std::sin(ph0));
            const bool on = t >= from && t < to;
            x[i] = (on ? (los + sc * s) * car : cf32(0, 0)) + cg(rng, sg, sg);
        }
        return x;
    }
};
static double fact(const X40Telemetry& t, const char* key, const char* f) { double v = -999; for (auto& q : t.facts) if (q.first == key) { const char* p = q.second.c_str(); if (strstr(f, "after:")) {} sscanf(p, f, &v); } return v; }

int main() {
    const double fs = 16000, noise = noiseFor(60, 1.0, fs) * 0;   // noise set per case below
    (void)noise;
    // 1. Rayleigh (K = 0) at fd 5 and 50 Hz against theory: fade rate 0.717 fd, fade length 0.133 / fd, Doppler spread fd / sqrt(2)
    for (double fd : {5.0, 50.0}) {
        Chan c{fs, fd, 0.0, 800, 3.0, 1e-4}; auto t = runChunks(x40::Fading, fs, 75.0, std::ref(c), 0, 144.4e6, 1.0);
        const double lcr = fact(t, "fade rate at -10 dB", "%lf"), afd = fact(t, "average fade length at -10 dB", "%lf") / 1000, dopp = fact(t, "Doppler spread", "%lf");
        const double kdb = [&] { for (auto& q : t.facts) if (q.first == "Rician K-factor") return q.second.rfind("0 (Rayleigh", 0) == 0 ? -99.0 : atof(q.second.c_str()); return 99.0; }();
        printf("  Rayleigh fd %.0f Hz: fade rate %.2f (theory %.2f), fade length %.1f ms (theory %.1f), Doppler spread %.1f Hz (theory %.1f), K %s\n", fd, lcr, 0.717 * fd, afd * 1000, 0.1328 / fd * 1000, dopp, fd / std::sqrt(2.0), kdb < -50 ? "0" : std::to_string(kdb).c_str());
        CHECK(std::fabs(lcr - 0.717 * fd) < 0.3 * 0.717 * fd, "fd %.0f: fade rate %.2f want %.2f", fd, lcr, 0.717 * fd);
        CHECK(std::fabs(afd - 0.1328 / fd) < 0.35 * 0.1328 / fd, "fd %.0f: fade length %.4f want %.4f", fd, afd, 0.1328 / fd);
        CHECK(std::fabs(dopp - fd / std::sqrt(2.0)) < 0.3 * fd / std::sqrt(2.0), "fd %.0f: Doppler spread %.1f want %.1f", fd, dopp, fd / std::sqrt(2.0));
        CHECK(kdb < 0, "fd %.0f: Rayleigh read as K %.1f dB", fd, kdb);
        CHECK(std::fabs(factNum(t, "scintillation index S4") - 1.0) < 0.12, "fd %.0f: S4 %.3f want 1.0", fd, factNum(t, "scintillation index S4"));
    }
    // 2. Rician: K = 10 dB and 3 dB at fd 8 Hz, offset and drift
    for (double kdb : {10.0, 3.0}) {
        Chan c{fs, 8.0, std::pow(10.0, kdb / 10), -1700, 2.0, 1e-4}; auto t = runChunks(x40::Fading, fs, 75.0, std::ref(c), 0, 144.4e6, 1.0);
        double rd = 99; for (auto& q : t.facts) if (q.first == "Rician K-factor") rd = atof(q.second.c_str());
        printf("  Rician K %.0f dB put in: read %.1f dB\n", kdb, rd);
        CHECK(std::fabs(rd - kdb) < 2.0, "K %.0f dB read %.1f", kdb, rd);
        { const double K = std::pow(10.0, kdb / 10), want = std::sqrt((2 * K + 1) / ((K + 1) * (K + 1))); CHECK(std::fabs(factNum(t, "scintillation index S4") - want) < 0.07, "S4 %.3f want %.3f", factNum(t, "scintillation index S4"), want); }
    }
    // 3. a clean carrier: no fading
    { Chan c{fs, 5.0, 1e6, 400, 0, 1e-4}; auto t = runChunks(x40::Fading, fs, 50.0, std::ref(c), 0, 144.4e6, 1.0); CHECK(factStr(t, "Rician K-factor").rfind("above 40 dB", 0) == 0, "clean carrier: %s", factStr(t, "Rician K-factor").c_str()); }
    // 4. noise sweep: Rician K = 10 dB, fd 8 Hz; carrier to noise in 1 kHz
    printf("noise sweep (carrier to noise in 1 kHz -> K read, true 10 dB):");
    for (double cnr : {40.0, 30.0, 25.0, 20.0, 15.0}) {
        Chan c{fs, 8.0, 10.0, 0, 0, noiseFor(cnr, 1000, fs)}; auto t = runChunks(x40::Fading, fs, 75.0, std::ref(c), 0, 144.4e6, 1.0);
        double rd = 99; for (auto& q : t.facts) if (q.first == "Rician K-factor") rd = atof(q.second.c_str()); printf(" %.0f->%.1f", cnr, rd);
        if (cnr >= 25) CHECK(std::fabs(rd - 10.0) < 2.5, "%.0f dB: K %.1f", cnr, rd);
    }
    printf("\n");
    // 5. carrier lost and found again
    { Chan c{fs, 5.0, 3.0, 0, 0, 1e-4}; (void)0;
      auto gen = [&](double t0, size_t n) { auto x = c(t0, n); if (t0 >= 60 && t0 < 80) for (auto& v : x) v *= 0.0f; return x; };
      auto t = runChunks(x40::Fading, fs, 150.0, gen, 0, 144.4e6, 1.0); CHECK(countKind(t, "Carrier") >= 3, "carrier rows %d (found, lost, found)", countKind(t, "Carrier")); }
    // 6. noise alone
    { ToneScene sc(fs, 1.0, 5); auto t = runChunks(x40::Fading, fs, 90.0, std::ref(sc), 0, 144.4e6, 1.0); CHECK(t.rows.empty(), "noise: %zu rows", t.rows.size()); }
    printf(fails ? "x40 fading: %d FAILED\n" : "x40 fading: ok\n", fails);
    return fails ? 1 : 0;
}
