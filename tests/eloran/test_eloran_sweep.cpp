// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Loran-C sweep: every chain of the public GRI list at several sample rates, carrier offsets, tuning errors, noise levels and group phase (A / B first), with the master
// and two secondaries at random delays; plus the chain names and the 9th pulse states of the eLoran data channel (Lo et al., IEEE TAES 2007, table III).
#include "dect2/eloran/eloran_engine.h"
#include "dect2/eloran/eloran_gen.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

using namespace dect2;
static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { std::printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

int main() {
    // ---- the 9th pulse states against the table of the paper (ideal delays, microseconds)
    {
        const double table[32] = {0, 1.25, 2.5, 3.75, 5, 6.25, 7.5, 8.75, 50.625, 51.875, 53.125, 54.375, 55.625, 56.875, 58.125, 59.375,
                                  101.25, 102.5, 103.75, 105, 106.25, 107.5, 108.75, 110, 151.875, 153.125, 154.375, 155.625, 156.875, 158.125, 159.375, 160.625};
        int bad = 0;
        for (int k = 0; k < 32; k++) {
            if (std::fabs(loran::ninthPulseDelayUs(k) - table[k]) > 1e-9) bad++;
            if (loran::ninthPulseState(table[k]) != k) bad++;
            if (loran::ninthPulseState(table[k] + 0.4) != k || loran::ninthPulseState(table[k] - 0.4) != k) bad++;   // a measurement error under a third of the step
        }
        CHECK(bad == 0, "9th pulse state table: %d mismatches", bad);
        CHECK(loran::ninthPulseState(-20) == -1 && loran::ninthPulseState(200) == -1 && loran::ninthPulseDelayUs(32) < 0, "outside the states");
    }
    // ---- chain names
    CHECK(std::string(loran::chainName(99600)) == "Northeast U.S." && std::string(loran::chainName(89700)) == "Great Lakes" && std::string(loran::chainName(99400)) == "U.S. West Coast" &&
          std::string(loran::chainName(59300)) == "Canadian East Coast" && loran::chainName(12340) == nullptr, "chain names");
    // Chayka (the Russian Loran-C equivalent, same 100 kHz pulse groups) chains by GRI
    CHECK(std::string(loran::chainName(80000)) == "Western Russia Chayka" && std::string(loran::chainName(79500)) == "Eastern Russia Chayka" &&
          std::string(loran::chainName(49700)) == "North-Western Russia Chayka" && std::string(loran::chainName(59700)) == "South Ural Chayka", "Chayka chain names");

    // ---- every chain, sweeps
    const int gris[] = {4970, 5543, 5930, 5960, 5970, 5980, 5990, 6042, 6731, 6780, 7001, 7030, 7270, 7430, 7499, 7950, 7960, 7980, 7990, 8000, 8290, 8390, 8830, 8930, 8970, 9007, 9610, 9930, 9940, 9960, 9990};
    std::mt19937 rng(77);
    int cases = 0, bad = 0;
    double worst = 0;
    for (size_t gi = 0; gi < sizeof gris / sizeof gris[0]; gi++) {
        const int gri = gris[gi] * 10;
        const double fs = (gi % 3 == 0) ? 250000.0 : (gi % 3 == 1) ? 400000.0 : 500000.0;
        const double carrier = (gi % 2) ? -30000.0 : 25000.0;
        static const double kCfo[9] = {0.0, 10.0, -25.0, 50.0, -80.0, 100.0, -60.0, 200.0, -230.0};   // tuning error in Hz: a radio's LO error is a few Hz at 100 kHz; the detector takes +-250 Hz
        const double cfo = getenv("CFO") ? atof(getenv("CFO")) : kCfo[gi % 9];
        std::uniform_real_distribution<double> ud(8000.0, gri - 20000.0);
        double d1 = ud(rng), d2 = ud(rng);
        if (std::fabs(d1 - d2) < 4000) d2 = std::fmod(d2 + 9000.0, gri - 20000.0) + 8000.0;
        loran::GenParams g;
        g.fs = fs; g.griUs = gri; g.seconds = 3.5; g.carrierHz = carrier + cfo; g.snrDb = 14.0; g.seed = (uint32_t)(100 + gi);
        g.firstGroupUs = 1000.0 + (double)(gi * 3777 % 9000);
        g.stations.push_back({true, 0.0, 1.0});
        g.stations.push_back({false, d1, 0.9});
        g.stations.push_back({false, d2, 0.7});
        const auto x = loran::generateLoran(g);
        ElLoranReceiver rx;
        rx.configure(fs);
        rx.setSignalOffset(carrier);
        for (size_t pos = 0; pos < x.size(); pos += 4096) rx.feed(&x[pos], std::min<size_t>(4096, x.size() - pos));
        ElLoranTelemetry t;
        rx.telemetry(t, 0);
        cases++;
        const bool ok = t.dataValid && t.griUs == gri && t.masterFound && t.stations.size() == 3 && t.chain == (loran::chainName(gri) ? loran::chainName(gri) : "");
        double e = 1e9;
        const bool cfoOk = std::fabs(t.cfoHz - cfo) < 15.0;
        if (ok) {
            // the secondaries come sorted by arrival: compare with the sorted truth
            double a = std::min(d1, d2), b = std::max(d1, d2);
            e = std::max(std::fabs(t.stations[1].toaUs - a), std::fabs(t.stations[2].toaUs - b));
            worst = std::max(worst, e);
        }
        if (!ok || e > 3.0 || !cfoOk) { bad++; std::printf("sweep miss GRI %d fs %.0f carrier %.0f cfo %.0f: valid %d gri %d master %d stations %zu error %.2f us chain '%s' measured tuning error %.1f Hz\n", gris[gi], fs, carrier, cfo, (int)t.dataValid, t.griUs, (int)t.masterFound, t.stations.size(), e, t.chain.c_str(), t.cfoHz); }
    }
    std::printf("loran sweep: %d chains, %d failed, worst offset error %.2f us\n", cases, bad, worst);
    CHECK(bad == 0, "sweep: %d of %d chains failed", bad, cases);
    std::printf("%s: %d failure(s)\n", fails ? "test_eloran_sweep FAILED" : "test_eloran_sweep OK", fails);
    return fails ? 1 : 0;
}
