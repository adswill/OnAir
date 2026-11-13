// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Frequency-hopper analyser: hop rate, dwell, channel count and spacing against a generated hopper (random channel order, FSK inside the dwell, guard time, jitter in the hop clock),
// with a radio offset, a drifting radio, a noise sweep, and the false reports for noise, a steady carrier and a chirp. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Hopper {
    double fs, centre, spacing, dwell, guard, jitter, ppm, lo, drift, noise, amp = 1; int nCh; Rng rng{11}; std::mt19937 g{5};
    double ph = 0, tNext = 0, tEnd = 0, f = 0, fskPh = 0; bool init = false; double fskHz = 0;
    std::vector<cf32> operator()(double t0, size_t n) {
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        for (size_t i = 0; i < n; i++) {
            const double t = t0 + i / fs;
            if (!init || t >= tNext) {
                init = true; const int ch = (int)(g() % (unsigned)nCh); f = centre + (ch - (nCh - 1) / 2.0) * spacing;
                const double d = dwell * (1 + ppm * 1e-6) * (1 + jitter * ((double)(g() % 2001) / 1000.0 - 1.0));
                tEnd = t + d - guard; tNext = t + d;
            }
            const bool on = t < tEnd;
            if (((long)(t * 10000)) != ((long)((t - 1.0 / fs) * 10000))) fskHz = ((g() & 1) ? 1 : -1) * 3000.0;      // 10 kbit/s FSK, +-3 kHz
            ph += 2 * kPi * (f + fskHz + lo + drift * t) / fs;
            x[i] = (on ? (float)amp * cf32((float)std::cos(ph), (float)std::sin(ph)) : cf32(0, 0)) + cg(rng, sg, sg);
        }
        return x;
    }
};
static double fact(const X40Telemetry& t, const char* key, const char* fmtS) { double v = -1; for (auto& f : t.facts) if (f.first == key) sscanf(f.second.c_str(), fmtS, &v); return v; }

int main() {
    const double fs = 2000000;
    struct Case { int nCh; double spacing, dwell, guard, jitter, ppm, lo, drift; };
    for (Case c : {Case{16, 50000, 0.002, 0.0003, 0.0, 0, 0, 0}, Case{8, 100000, 0.010, 0.001, 0.02, 200, 3000, 20}, Case{32, 25000, 0.00125, 0.0002, 0.0, -300, -1500, 0}, Case{12, 60000, 0.0004 * 5, 0.0003, 0.03, 0, 0, 0}}) {
        Hopper h{fs, 0, c.spacing, c.dwell, c.guard, c.jitter, c.ppm, c.lo, c.drift, noiseFor(30, 25000, fs)}; h.nCh = c.nCh;
        auto t = runChunks(x40::HopAnalyser, fs, 4.0, std::ref(h), 0, 433.9e6, 0.5);
        const double rate = fact(t, "hop rate", "%lf"), spacing = fact(t, "channel spacing", "%lf") * 1000, chs = fact(t, "channels", "%lf"), dwell = fact(t, "dwell", "%lf") / 1000;
        const double wantRate = 1.0 / (c.dwell * (1 + c.ppm * 1e-6));
        printf("  %d ch, %.0f kHz, dwell %.2f ms: rate %.1f (true %.1f), channels %.0f, spacing %.2f kHz, dwell %.2f ms\n", c.nCh, c.spacing / 1000, c.dwell * 1000, rate, wantRate, chs, spacing / 1000, dwell * 1000);
        CHECK(std::fabs(rate - wantRate) < 0.03 * wantRate, "rate %.1f want %.1f (%s)", rate, wantRate, t.headline.c_str());
        CHECK((int)chs == c.nCh, "channels %.0f want %d", chs, c.nCh);
        CHECK(std::fabs(spacing - c.spacing) < 0.04 * c.spacing, "spacing %.0f want %.0f", spacing, c.spacing);
        CHECK(std::fabs(dwell - (c.dwell - c.guard)) < std::max(0.0002, 0.3 * c.dwell), "dwell %.4f want about %.4f", dwell, c.dwell - c.guard);
        CHECK(countKind(t, "Hopper") >= 1, "no Hopper row");
    }
    // noise sweep: 16 channels, 2 ms dwell; carrier to noise in 25 kHz
    printf("noise sweep (carrier to noise in 25 kHz): found / rate / channels\n");
    for (double cnr : {30.0, 20.0, 15.0, 12.0, 9.0, 6.0}) {
        Hopper h{fs, 0, 50000, 0.002, 0.0003, 0, 0, 0, 0, noiseFor(cnr, 25000, fs)}; h.nCh = 16;
        auto t = runChunks(x40::HopAnalyser, fs, 4.0, std::ref(h), 0, 433.9e6, 0.5);
        const bool ok = countKind(t, "Hopper") >= 1; printf("  %.0f dB: %s rate %.0f channels %.0f\n", cnr, ok ? "yes" : "no", fact(t, "hop rate", "%lf"), fact(t, "channels", "%lf"));
        if (cnr >= 12) CHECK(ok && (int)fact(t, "channels", "%lf") == 16, "%.0f dB: found %d", cnr, (int)ok);
    }
    // false reports: noise, a steady carrier, a slow chirp (a swept signal), a two-channel toggler (fewer than 3 channels), 8 s each
    {
        int falseRows = 0;
        { ToneScene sc(fs, 1.0, 3); falseRows += (int)runChunks(x40::HopAnalyser, fs, 8.0, std::ref(sc), 0, 433.9e6, 0.5).rows.size(); }
        { ToneScene sc(fs, noiseFor(30, 25000, fs), 4); SchedTone s; s.hz = 123456; sc.tones.push_back(s); falseRows += (int)runChunks(x40::HopAnalyser, fs, 8.0, std::ref(sc), 0, 433.9e6, 0.5).rows.size(); }
        { ToneScene sc(fs, noiseFor(30, 25000, fs), 5); SchedTone s; s.hz = -400000; s.driftHzPerSec = 100000; sc.tones.push_back(s); falseRows += (int)runChunks(x40::HopAnalyser, fs, 8.0, std::ref(sc), 0, 433.9e6, 0.5).rows.size(); }
        { Hopper h{fs, 0, 80000, 0.003, 0.0003, 0, 0, 0, 0, noiseFor(30, 25000, fs)}; h.nCh = 2; falseRows += (int)runChunks(x40::HopAnalyser, fs, 8.0, std::ref(h), 0, 433.9e6, 0.5).rows.size(); }
        printf("noise, carrier, chirp, two-channel toggler, 4 x 8 s: %d false reports\n", falseRows);
        CHECK(falseRows == 0, "false reports %d", falseRows);
    }
    printf(fails ? "x40 hop: %d FAILED\n" : "x40 hop: ok\n", fails);
    return fails ? 1 : 0;
}
