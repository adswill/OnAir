// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// eLoran / Loran-C: the detector finds the GRI, the master and two secondaries and their offsets (within 2 us at 18 dB, scatter of a few us at 0 dB, see below).
// The synthetic signal is built from the public pulse shape (t^2 exp(-2t/65 us)) and the public phase codes (eloran_gen.h).
#include "dect2/eloran/eloran_engine.h"
#include "dect2/eloran/eloran_gen.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace dect2;

static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { std::printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

static loran::GenParams chain(double fs, int gri, double snr, double seconds, uint32_t seed) {
    loran::GenParams g;
    g.fs = fs;
    g.griUs = gri;
    g.seconds = seconds;
    g.carrierHz = -30000.0;
    g.snrDb = snr;
    g.seed = seed;
    g.stations.push_back({true, 0.0, 1.0});
    g.stations.push_back({false, 23456.7, 0.9});
    g.stations.push_back({false, 41234.5, 0.8});
    return g;
}

// returns the larger of the two offset errors (us); checks the GRI, the stations and the phase codes
static double checkTelemetry(const ElLoranTelemetry& t, int gri, const char* what) {
    CHECK(t.dataValid, "%s: no GRI", what);
    CHECK(t.griUs == gri, "%s: GRI %d, want %d", what, t.griUs, gri);
    CHECK(t.masterFound, "%s: master not found", what);
    CHECK(t.stations.size() == 3, "%s: %d stations, want 3", what, (int)t.stations.size());
    if (t.stations.size() != 3) return 1e9;
    CHECK(t.stations[0].master && t.stations[0].toaUs == 0.0, "%s: master toa %.2f", what, t.stations[0].toaUs);
    CHECK(t.stations[0].codeMaster && !t.stations[1].codeMaster && !t.stations[2].codeMaster, "%s: phase codes master/secondary wrong", what);
    CHECK(!t.pulseEnv.empty(), "%s: no pulse envelope", what);
    CHECK(t.locked || t.state == 2, "%s: state %d", what, t.state);
    return std::max(std::fabs(t.stations[1].toaUs - 23456.7), std::fabs(t.stations[2].toaUs - 41234.5));
}

static ElLoranTelemetry runReceiver(double inRate, int gri, double snr, double seconds, uint32_t seed) {
    const std::vector<std::complex<float>> x = loran::generateLoran(chain(inRate, gri, snr, seconds, seed));
    ElLoranReceiver rx;
    rx.configure(inRate);
    rx.setSignalOffset(-30000.0);
    size_t pos = 0;
    while (pos < x.size()) {
        const size_t n = std::min<size_t>(4096, x.size() - pos);
        rx.feed(&x[pos], n);
        pos += n;
    }
    ElLoranTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

int main() {
    // 1. the detector alone, no noise, 2 s
    {
        loran::GenParams g = chain(250000.0, 99600, 1e9, 2.0, 1);
        g.carrierHz = 0.0;   // the detector alone expects the carrier at 0 Hz
        const std::vector<std::complex<float>> x = loran::generateLoran(g);
        loran::Detector det(g.fs);
        const loran::Result r = det.analyze(x.data(), x.size());
        CHECK(r.found && r.griUs == 99600, "clean: GRI %d", r.griUs);
        CHECK(r.stations.size() == 3, "clean: %d stations", (int)r.stations.size());
        if (r.stations.size() == 3) {
            CHECK(std::fabs(r.stations[1].toaUs - 23456.7) < 0.2, "clean: toa 1 %.3f", r.stations[1].toaUs);
            CHECK(std::fabs(r.stations[2].toaUs - 41234.5) < 0.2, "clean: toa 2 %.3f", r.stations[2].toaUs);
            CHECK(r.masterFound && r.stations[0].master, "clean: master");
            CHECK(r.stations[0].code == 'A' || r.stations[0].code == 'B', "clean: code");
        }
        // the pulse envelope peaks 65 us after the pulse start (50 us before the start is the first sample)
        size_t pk = 0;
        for (size_t i = 1; i < r.pulseEnv.size(); i++) if (r.pulseEnv[i] > r.pulseEnv[pk]) pk = i;
        const double peakUs = r.envStartUs + (double)pk * r.envStepUs;
        CHECK(std::fabs(peakUs - 65.0) < 12.0, "clean: pulse peak at %.1f us, want about 65", peakUs);
    }
    // 2. SNR 0 dB (peak pulse power over the noise power in 20 kHz), 3.5 s: everything is found; the offsets scatter by about
    //    3 us rms, which is what the Cramer-Rao bound gives for 20 groups at this SNR (see the report), so the bound here is 15 us
    {
        double sumsq = 0, worst = 0;
        int n = 0;
        for (uint32_t seed = 1; seed <= 4; seed++) {
            const double e = checkTelemetry(runReceiver(250000.0, 99600, 0.0, 3.5, seed), 99600, "0 dB, 250 ksps, GRI 9960");
            sumsq += e * e; n++; worst = std::max(worst, e);
        }
        for (uint32_t seed = 11; seed <= 13; seed++) {
            const double e = checkTelemetry(runReceiver(1000000.0, 79700, 0.0, 3.5, seed), 79700, "0 dB, 1 Msps, GRI 7970");
            sumsq += e * e; n++; worst = std::max(worst, e);
        }
        std::printf("  0 dB: worst offset error %.2f us, rms of the worse of two %.2f us over %d runs\n", worst, std::sqrt(sumsq / n), n);
        CHECK(worst < 15.0, "0 dB: worst offset error %.2f us", worst);
        CHECK(std::sqrt(sumsq / n) < 7.0, "0 dB: rms offset error %.2f us", std::sqrt(sumsq / n));
    }
    // 3. SNR 18 dB: the offsets are within 2 us
    {
        double worst = 0;
        for (uint32_t seed = 21; seed <= 23; seed++) worst = std::max(worst, checkTelemetry(runReceiver(250000.0, 99600, 18.0, 3.5, seed), 99600, "18 dB, 250 ksps"));
        for (uint32_t seed = 31; seed <= 32; seed++) worst = std::max(worst, checkTelemetry(runReceiver(500000.0, 59900, 18.0, 3.5, seed), 59900, "18 dB, 500 ksps, GRI 5990"));
        std::printf("  18 dB: worst offset error %.2f us\n", worst);
        CHECK(worst < 2.0, "18 dB: worst offset error %.2f us", worst);
    }
    // 4. noise only: nothing is found (8 runs)
    for (uint32_t seed = 1; seed <= 8; seed++) {
        loran::GenParams g = chain(250000.0, 99600, 0.0, 3.5, seed);
        g.stations.clear();
        const std::vector<std::complex<float>> x = loran::generateLoran(g);
        ElLoranReceiver rx;
        rx.configure(250000.0);
        rx.setSignalOffset(-30000.0);
        rx.feed(x.data(), x.size());
        ElLoranTelemetry t;
        const bool got = rx.telemetry(t, 0);
        CHECK(got && !t.dataValid && t.griUs == 0, "noise only (seed %u): GRI %d found", seed, t.griUs);
    }
    std::printf("%s: %d failure(s)\n", fails ? "test_eloran_detect FAILED" : "test_eloran_detect OK", fails);
    return fails ? 1 : 0;
}
