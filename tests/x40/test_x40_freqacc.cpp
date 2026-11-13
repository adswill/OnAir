// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Carrier frequency accuracy meter: the offset and ppm read against a generated carrier, with offsets, a drifting carrier, steps, loss and return, a noise sweep (C/N0 and the
// scatter of the reading), the Allan deviation of a carrier with a known random walk, and false readings on noise. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    // 1. offsets, rates and the tuned frequency: the reading is within 0.1 Hz at 50 dB-Hz
    for (double fs : {48000.0, 96000.0, 250000.0}) for (double off : {0.0, 12.34, -1730.5, 2400.0}) {
        const double centre = 10e6;
        ToneScene sc(fs, noiseFor(30, 1000, fs), 3);   // 30 dB in 1 kHz: C/N0 60 dB-Hz
        SchedTone s; s.hz = off; sc.tones.push_back(s);
        auto t = runChunks(x40::FreqAcc, fs, 20.0, std::ref(sc), 0, centre);
        const double o = factNum(t, "offset");
        CHECK(std::fabs(o - off) < 0.1, "fs %.0f off %.2f: read %.3f (%s)", fs, off, o, t.headline.c_str());
        CHECK(std::fabs(factNum(t, "error") - off / centre * 1e6) < 0.01, "ppm %.4f want %.4f", factNum(t, "error"), off / centre * 1e6);
        CHECK(std::fabs(factNum(t, "carrier to noise density") - 60) < 2.0, "C/N0 %.1f want 60", factNum(t, "carrier to noise density"));
    }
    // 2. a drifting carrier (1 Hz/s): the drift is read in ppm per hour
    { ToneScene sc(48000, noiseFor(30, 1000, 48000), 4); SchedTone s; s.hz = 100; s.driftHzPerSec = 1.0; sc.tones.push_back(s);
      auto t = runChunks(x40::FreqAcc, 48000, 120.0, std::ref(sc), 0, 10e6);
      const double d = factNum(t, "drift"); const double want = 1.0 * 3600 / 10e6 * 1e6;
      CHECK(std::fabs(d - want) < 0.05 * want + 0.01, "drift %.3f ppm/h want %.3f", d, want); }
    // 3. a step of 3 Hz is logged once; a gap of 20 s is logged as lost and returned
    { ToneScene sc(48000, noiseFor(30, 1000, 48000), 5); SchedTone a; a.hz = 50; a.to = 40; sc.tones.push_back(a); SchedTone b; b.hz = 53; b.from = 40; sc.tones.push_back(b);
      auto t = runChunks(x40::FreqAcc, 48000, 80.0, std::ref(sc), 0, 10e6);
      CHECK(countKind(t, "Step") == 1, "steps %d", countKind(t, "Step")); }
    { ToneScene sc(48000, noiseFor(30, 1000, 48000), 6); SchedTone a; a.hz = 50; a.to = 30; sc.tones.push_back(a); SchedTone b; b.hz = 50; b.from = 50; sc.tones.push_back(b);
      auto t = runChunks(x40::FreqAcc, 48000, 80.0, std::ref(sc), 0, 10e6);
      CHECK(countKind(t, "Carrier") == 2, "lost / returned %d", countKind(t, "Carrier")); }
    // 4. noise sweep: the scatter of the readings and whether the carrier is found
    printf("noise sweep (C/N0 dB-Hz): offset read / true 37.00 Hz, scatter\n");
    for (double cn0 : {60.0, 50.0, 40.0, 35.0, 30.0, 27.0}) {
        ToneScene sc(48000, noiseFor(cn0 - 30, 1.0, 48000) * 1.0, 8);   // amplitude 1: C/N0 = 10 log(fs / noise)
        sc.noisePower = 48000.0 / std::pow(10.0, cn0 / 10); SchedTone s; s.hz = 37.0; sc.tones.push_back(s);
        auto t = runChunks(x40::FreqAcc, 48000, 60.0, std::ref(sc), 0, 10e6);
        const std::string f = factStr(t, "offset");
        printf("  %.0f: %s\n", cn0, f.empty() ? "no carrier" : f.c_str());
        if (cn0 >= 35) CHECK(std::fabs(factNum(t, "offset") - 37.0) < 0.3, "%.0f dB-Hz: %.2f", cn0, factNum(t, "offset"));
    }
    // 5. Allan deviation of a carrier with white frequency noise of 0.2 Hz per reading window (1.024 s): at 1 s it must read 0.2 Hz / 10 MHz = 2e-8 within 30 %
    {
        const double fs = 48000; Rng r(5); std::vector<double> fq; for (int i = 0; i < 400; i++) fq.push_back(100 + 0.2 * r.g());
        double ph = 0; Rng nz(9); const float sg = (float)std::sqrt(48000.0 / std::pow(10.0, 7.0) / 2);
        auto gen = [&](double t0, size_t n) { std::vector<cf32> x(n); for (size_t i = 0; i < n; i++) { const double t = t0 + i / fs; const int k = std::max(0, std::min(399, (int)((t - 0.2) / (16384 / 16000.0))));   /* the readings' windows start 0.2 s in */ ph += 2 * kPi * fq[k] / fs; x[i] = cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(sg * nz.g(), sg * nz.g()); } return x; };
        auto t = runChunks(x40::FreqAcc, fs, 800.0, gen, 0, 10e6, 1.0);
        const std::string av = factStr(t, "Allan deviation"); double a2 = 0; sscanf(av.c_str(), "1.0 s: %lf", &a2);
        CHECK(a2 > 1.4e-8 && a2 < 2.6e-8, "Allan 2 s %.2e (%s)", a2, av.c_str());
    }
    // 6. noise only for 10 minutes: no carrier, no rows
    { ToneScene sc(48000, 1.0, 2); auto t = runChunks(x40::FreqAcc, 48000, 600.0, std::ref(sc), 0, 10e6, 1.0); CHECK(t.rows.empty(), "false rows %zu", t.rows.size()); }
    printf(fails ? "x40 freqacc: %d FAILED\n" : "x40 freqacc: ok\n", fails);
    return fails ? 1 : 0;
}
