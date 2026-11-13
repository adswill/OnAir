// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM modulation monitor: depth, peaks, asymmetry, hum, audio bandwidth, carrier offset and the over-modulation and carrier-loss logs against what the generator put in; input rates,
// offsets with drift, a noise sweep and a noise-only run. Simulated signals only.
#include "x40_gen2.h"
#include <cstdlib>
#include <cstring>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::string factS(const X40Telemetry& t, const char* key) { for (auto& f : t.facts) if (f.first == key) return f.second; return ""; }
static double fact(const X40Telemetry& t, const char* key) { return atof(factS(t, key).c_str()); }
static double second(const X40Telemetry& t, const char* key) { const auto s = factS(t, key); const auto p = s.find('/'); return p == std::string::npos ? -999 : std::fabs(atof(s.c_str() + p + 1)); }
static double after(const std::string& s, const char* w) { const auto p = s.find(w); return p == std::string::npos ? -1 : atof(s.c_str() + p + strlen(w)); }

int main() {
    for (double fs : {96000.0, 250000.0, 1000000.0}) for (double off : {0.0, 350.0, -2200.0}) {
        Am a; a.offsetHz = off; a.driftHzPerSec = off == 0 ? 3 : 0;
        auto t = run(x40::AmMon, amStation(a, fs, 5.0), fs);
        // modulation: sine equivalent of sqrt(0.5^2 + 0.2^2) = 0.539
        CHECK(std::fabs(fact(t, "modulation (RMS, as a sine)") - 53.9) < 3.0, "fs %.0f off %.0f depth %.1f", fs, off, fact(t, "modulation (RMS, as a sine)"));
        CHECK(std::fabs(fact(t, "carrier offset") - (off + (off == 0 ? 3 * 3.6 : 0))) < 1.5, "offset %.2f want %.0f", fact(t, "carrier offset"), off);
        const auto pn = factS(t, "positive / negative peaks");
        CHECK(std::fabs(atof(pn.c_str() + 1) - 69) < 6 && std::fabs(second(t, "positive / negative peaks") - 69) < 6, "peaks %s", pn.c_str());
        CHECK(fact(t, "over-modulation") < 0.1, "over %s", factS(t, "over-modulation").c_str());
        CHECK(factS(t, "mains hum") == "none seen", "hum %s", factS(t, "mains hum").c_str());
        const double bw = after(factS(t, "audio bandwidth"), "below ");
        CHECK(bw > 3.2 && bw < 4.5, "bw %.2f (%s)", bw, factS(t, "audio bandwidth").c_str());
    }
    { Am a; a.hum = 0.03; a.humHz = 100; auto t = run(x40::AmMon, amStation(a, 250000, 5.0), 250000); const double h = after(factS(t, "mains hum"), "") ;   // 3 % of the carrier = -30.5 dB
      CHECK(factS(t, "mains hum").find("at 100 Hz") != std::string::npos && std::fabs(h - 30.5) < 2.0 /* printed as a positive number of dB */, "hum %s", factS(t, "mains hum").c_str());
      Am b; b.hum = 0.006; b.humHz = 60; auto u = run(x40::AmMon, amStation(b, 250000, 5.0), 250000); CHECK(factS(u, "mains hum").find("at 60 Hz") != std::string::npos && std::fabs(after(factS(u, "mains hum"), "") - 44.4) < 2.5, "hum60 %s", factS(u, "mains hum").c_str()); }
    // asymmetry
    { Am a; a.asym = 0.6; auto t = run(x40::AmMon, amStation(a, 250000, 5.0), 250000); const double as = fact(t, "asymmetry"); CHECK(as > 5 && as < 40, "asymmetry %.0f", as); }
    // over-modulation: 130 % peaks clip at zero carrier
    { Am a; a.d1 = 1.0; a.d2 = 0.4; auto t = run(x40::AmMon, amStation(a, 250000, 5.0), 250000); CHECK(countKind(t, "Over-modulation") >= 3, "over rows %d", countKind(t, "Over-modulation")); }
    { Am a; auto t = run(x40::AmMon, amStation(a, 250000, 5.0), 250000); CHECK(countKind(t, "Over-modulation") == 0, "false over-mod"); }
    // the carrier drops out for 5 s and comes back
    { Am a; a.cutFrom = 5; a.cutTo = 11; auto x = amStation(a, 250000, 16.0); addNoise(x, noiseFor(60, 12000, 250000), 3); auto t = run(x40::AmMon, x, 250000);
      CHECK(countKind(t, "Carrier") == 3, "carrier rows %d", countKind(t, "Carrier")); }
    // noise
    printf("noise sweep (carrier to noise in 12 kHz): modulation read / 53.9 %%, offset, rows\n");
    for (double cnr : {50.0, 40.0, 30.0, 20.0, 10.0}) {
        Am a; a.offsetHz = 120; auto x = amStation(a, 250000, 5.0); addNoise(x, noiseFor(cnr, 12000, 250000), 8); auto t = run(x40::AmMon, x, 250000);
        printf("  %.0f dB: modulation %.1f %%, offset %.2f Hz, over-mod rows %d, hum '%s'\n", cnr, fact(t, "modulation (RMS, as a sine)"), fact(t, "carrier offset"), countKind(t, "Over-modulation"), factS(t, "mains hum").c_str());
        if (cnr >= 30) { CHECK(std::fabs(fact(t, "modulation (RMS, as a sine)") - 53.9) < 4, "mod at %.0f dB", cnr); CHECK(std::fabs(fact(t, "carrier offset") - 120) < 2, "offset at %.0f dB", cnr); CHECK(factS(t, "mains hum") == "none seen", "hum at %.0f dB", cnr); }
    }
    {   // an unmodulated carrier in noise: no over-modulation, no hum, no carrier change in 10 minutes
        int falseRows = 0, humSeen = 0;
        for (int k = 0; k < 10; k++) {
            std::vector<cf32> x((size_t)(96000 * 60), cf32(1, 0)); addNoise(x, noiseFor(k < 5 ? 40.0 : 25.0, 12000, 96000), 70 + k);
            auto t = run(x40::AmMon, x, 96000);
            for (auto& r : t.rows) if (!(r.kind == "Carrier" && r.id == "present")) falseRows++;
            if (factS(t, "mains hum") != "none seen") humSeen++;
        }
        printf("plain carrier in noise (40 and 25 dB), 10 minutes: %d false rows, %d false hum\n", falseRows, humSeen);
        CHECK(falseRows == 0 && humSeen == 0, "false rows %d hum %d", falseRows, humSeen);
    }
    {   // white noise alone: no carrier, no pilot, no buzz (3.9 of the sweep: the AM monitor "found a carrier", the FM monitor a 4.5 kHz pilot, UVB-76 a buzz)
        for (double sg : {0.02, 0.1, 0.5}) {
            std::vector<cf32> x((size_t)(250000 * 8), cf32(0, 0)); addNoise(x, (float)sg, 5);
            auto t = run(x40::AmMon, x, 250000);
            CHECK(t.blocksOk == 0, "AM monitor on noise (sigma %.2f): %llu decodes", sg, (unsigned long long)t.blocksOk);
        }
    }

    printf(fails ? "x40 ammon: %d FAILED\n" : "x40 ammon: ok\n", fails);
    return fails ? 1 : 0;
}
