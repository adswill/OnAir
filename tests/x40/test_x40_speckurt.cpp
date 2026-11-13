// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spectral kurtosis RFI finder: Gaussian noise gives SK = 1 and a clean band; a steady carrier reads SK near 0; a pulsed carrier reads SK near 1 / q - 1 (q the share of the 0.5 ms frames at full power), and a pulsed signal too weak to show in the
// averaged spectrum is still found; radio offset and noise sweeps; false regions on noise alone. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Reg { double mhz0, mhz1; bool pulsed; double sk; };
static std::vector<Reg> regions(const X40Telemetry& t) { std::vector<Reg> v; for (auto& f : t.facts) if (f.first.rfind("region", 0) == 0) { Reg r{}; char kind[20] = {0}; if (sscanf(f.second.c_str(), "%lf MHz to %lf MHz: %19[^,], SK %lf", &r.mhz0, &r.mhz1, kind, &r.sk) == 4) { r.pulsed = std::string(kind).find("pulsed") == 0; v.push_back(r); } } return v; }
static const Reg* at(const std::vector<Reg>& v, double mhz) { for (auto& r : v) if (mhz >= r.mhz0 - 0.004 && mhz <= r.mhz1 + 0.004) return &r; return nullptr; }

int main() {
    const double fs = 2e6, centre = 145e6;
    // 1. noise only: clean band, no region (60 s)
    { ToneScene sc(fs, 1.0, 3); auto t = runChunks(x40::SpecKurt, fs, 60.0, std::ref(sc), 0, centre, 1.0); CHECK(regions(t).empty() && t.rows.empty(), "noise: %zu regions, %zu rows", regions(t).size(), t.rows.size()); CHECK(factNum(t, "clean part of the band") > 99.0, "clean %.1f %%", factNum(t, "clean part of the band")); }
    // 2. a steady carrier at +300 kHz and a pulsed one (duty 10 %, period 4 ms) at -400 kHz, both 14 dB over the noise per bin; radio shifts of 0 and 777 Hz
    for (double shift : {0.0, 777.0}) {
        ToneScene sc(fs, 1.0, 4);
        { SchedTone s; s.hz = 300000 + shift; s.amp = 8.0; sc.tones.push_back(s); }                                  // 8^2 / (1 * 1024 bins' share) - per-bin SNR about 18 dB with the Hann gain
        { SchedTone s; s.hz = -400000 + shift; s.amp = 40.0; s.period = 0.004; s.duty = 0.1; sc.tones.push_back(s); }
        auto t = runChunks(x40::SpecKurt, fs, 60.0, std::ref(sc), 0, centre, 1.0);
        auto v = regions(t); const Reg* c = at(v, 145.3 + shift / 1e6), *p = at(v, 144.6 + shift / 1e6);
        CHECK(c && !c->pulsed && c->sk < 0.4, "shift %.0f: the steady carrier (%s)", shift, c ? "wrong class or SK" : "missing");
        CHECK(p && p->pulsed && p->sk > 2.5 && p->sk < 5.5, "shift %.0f: the pulsed carrier SK %.2f (want 3 - 5: 1 / q - 1 with q about 0.25 - 0.3 of the frames)", shift, p ? p->sk : -1.0);
        if (shift == 0 && p && c) printf("  steady carrier SK %.2f, pulsed (duty 10 %%, 0.4 ms in 4 ms) SK %.2f (about %.0f %% of the frames at full power)\n", c->sk, p->sk, 100.0 / (p->sk + 1));
        CHECK(countKind(t, "Pulsed") >= 1 && countKind(t, "Steady") >= 1, "rows: pulsed %d steady %d", countKind(t, "Pulsed"), countKind(t, "Steady"));
    }
    // 3. a pulsed carrier too weak to see in the averaged spectrum (duty 5 %, 0.6 ms in 12.3 ms): the mean level over the noise in its bin against what SK says
    printf("pulse amplitude (tone / noise per bin in a pulse frame, mean over the noise) -> SK, found:\n");
    for (double amp : {1.0, 0.5, 0.3, 0.2, 0.1, 0.07}) {
        ToneScene sc(fs, 1.0, 5); SchedTone s; s.hz = 123456; s.amp = amp; s.period = 0.0123; s.duty = 0.05; sc.tones.push_back(s);
        auto t = runChunks(x40::SpecKurt, fs, 60.0, std::ref(sc), 0, centre, 1.0); auto v = regions(t); const Reg* r = at(v, 145.1235);
        const double inPulse = 10 * std::log10(amp * amp * 682.7), meanOver = 10 * std::log10(1 + 0.05 * amp * amp * 682.7);
        printf("  %.2f (%.1f dB in a pulse frame, %.1f dB mean): %s\n", amp, inPulse, meanOver, r && r->pulsed ? std::to_string(r->sk).substr(0, 5).c_str() : "not flagged");
        if (amp >= 0.2) CHECK(r && r->pulsed, "pulse amp %.2f not found", amp);
    }
    { ToneScene sc(fs, 1.0, 6); SchedTone s; s.hz = 50000; s.amp = 8.0; s.period = 0.0071; s.duty = 0.3; sc.tones.push_back(s); auto t = runChunks(x40::SpecKurt, fs, 40.0, std::ref(sc), 0, centre, 1.0); auto v = regions(t); const Reg* r = at(v, 145.05); CHECK(r && r->pulsed && r->sk > 2.5 && r->sk < 6.5, "duty 30 %%: SK %.2f want 3 - 6", r ? r->sk : -1.0); }
    printf(fails ? "x40 speckurt: %d FAILED\n" : "x40 speckurt: ok\n", fails);
    return fails ? 1 : 0;
}
