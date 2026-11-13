// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Persistent carrier finder: steady, drifting, weak, keyed and frequency-modulated signals in a wide capture; the list must hold the persistent narrow ones with their frequency, drift and
// class, not the keyed one, and nothing on noise. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Item { double mhz = 0, level = 0, drift = 0, jitter = 0; bool bare = false; };
static std::vector<Item> items(const X40Telemetry& t) {
    std::vector<Item> v;
    for (auto& f : t.facts) if (f.first.rfind("carrier ", 0) == 0) { Item x; char kind[40] = {0}; double sd; int a, b;
        if (sscanf(f.second.c_str(), "%lf MHz, %lf dB over the noise (+-%lf dB), %39[^,], drift %lf Hz/min, scatter %lf Hz, present %d of %d", &x.mhz, &x.level, &sd, kind, &x.drift, &x.jitter, &a, &b) >= 6) { x.bare = std::string(kind).find("bare") == 0; v.push_back(x); } }
    return v;
}
static const Item* findNear(const std::vector<Item>& v, double hz, double centre, double tol = 8) { for (auto& x : v) if (std::fabs(x.mhz * 1e6 - (centre + hz)) < tol) return &x; return nullptr; }

int main() {
    const double fs = 1000000, centre = 145e6, noise = 2e-6;
    // 1. a bare carrier at +123456.7 Hz drifting 0.5 Hz/s, a steady one at -75000.3, a weak one (41 dB over the noise in its bin), a 50 % keyed one, a frequency-modulated carrier; radio shifts of 0 and +1234 Hz
    for (double shift : {0.0, 1234.0}) {
        ToneScene sc(fs, noise, 4);
        { SchedTone s; s.hz = 123456.7 + shift; s.amp = 0.05; s.driftHzPerSec = 0.5; sc.tones.push_back(s); }
        { SchedTone s; s.hz = -75000.3 + shift; s.amp = 0.02; sc.tones.push_back(s); }
        { SchedTone s; s.hz = 300000.0 + shift; s.amp = 0.0015; sc.tones.push_back(s); }       // 41 dB over the noise in its bin
        { SchedTone s; s.hz = -240000.0 + shift; s.amp = 0.05; s.period = 4; s.duty = 0.5; sc.tones.push_back(s); }
        double fmPh = 0; Rng fr(9); double lp = 0;
        auto gen = [&](double t0, size_t n) {
            auto x = sc(t0, n);
            for (size_t i = 0; i < n; i++) { lp += 0.02 * (fr.g() - lp); fmPh += 2 * kPi * (450000.0 + shift + 3000 * lp * 4) / fs; x[i] += 0.03f * cf32((float)std::cos(fmPh), (float)std::sin(fmPh)); }
            return x;
        };
        auto t = runChunks(x40::Birdie, fs, 100.0, gen, 0, centre, 0.5);
        auto v = items(t);
        const Item* a = findNear(v, 123456.7 + shift + 0.5 * 100 * 0.95, centre, 40), *b = findNear(v, -75000.3 + shift, centre, 6), *w = findNear(v, 300000.0 + shift, centre, 8);
        CHECK(a != nullptr, "shift %.0f: drifting carrier missing (%zu listed: %s)", shift, v.size(), t.headline.c_str());
        if (a) { CHECK(std::fabs(a->drift - 30.0) < 3.0, "drift %.1f Hz/min want 30", a->drift); CHECK(a->bare, "drifting carrier not 'bare'"); }
        CHECK(b != nullptr && b->bare && std::fabs(b->drift) < 1.0, "steady carrier: %s", b ? "listed but drifts or not bare" : "missing");
        CHECK(w != nullptr, "weak carrier (41 dB in its bin) missing");
        CHECK(findNear(v, -240000.0 + shift, centre, 400) == nullptr, "keyed carrier listed as persistent");
        for (auto& x : v) if (std::fabs(x.mhz * 1e6 - (centre + 450000 + shift)) < 15000) CHECK(!x.bare, "an FM signal listed as a bare carrier");
        CHECK(countKind(t, "Carrier") >= 3, "carrier rows %d", countKind(t, "Carrier"));
        if (shift == 0) printf("  listed %zu carriers, strongest %.0f dB, weak one %.0f dB over the noise\n", v.size(), v.empty() ? 0.0 : v[0].level, w ? w->level : 0.0);
    }
    // 2. a carrier that goes away is logged as gone, and returns
    { ToneScene sc(fs, noise, 5); SchedTone a; a.hz = 90000; a.amp = 0.03; a.to = 60; sc.tones.push_back(a); SchedTone b; b.hz = 90000; b.amp = 0.03; b.from = 100; sc.tones.push_back(b);
      auto t = runChunks(x40::Birdie, fs, 160.0, std::ref(sc), 0, centre, 0.5); CHECK(countKind(t, "Gone") == 1, "gone rows %d", countKind(t, "Gone")); CHECK(countKind(t, "Carrier") == 2, "carrier rows %d", countKind(t, "Carrier")); }
    // 3. level sweep: the weakest persistent carrier found (dB over the noise in the peak) and noise alone
    printf("carrier level over the noise (one 61 Hz bin of the averaged spectrum) -> found:");
    for (double amp : {0.003, 0.001, 0.0005, 0.0003, 0.0002, 0.00015, 0.0001}) {
        ToneScene sc(fs, noise, 6); SchedTone a; a.hz = 55555.5; a.amp = amp; sc.tones.push_back(a);
        auto t = runChunks(x40::Birdie, fs, 60.0, std::ref(sc), 0, centre, 0.5); const bool ok = findNear(items(t), 55555.5, centre, 8) != nullptr;
        const double db = 10 * std::log10(amp * amp * 8192.0 * 8192.0 / (noise * 6144.0));
        printf(" %.0f dB:%s", db, ok ? "yes" : "no");
        if (db >= 18) CHECK(ok, "%.0f dB not found", db);
    }
    printf("\n");
    { ToneScene sc(fs, 1.0, 3); auto t = runChunks(x40::Birdie, fs, 150.0, std::ref(sc), 0, centre, 0.5); CHECK(t.rows.empty(), "noise only: %zu rows", t.rows.size()); }
    printf(fails ? "x40 birdie: %d FAILED\n" : "x40 birdie: ok\n", fails);
    return fails ? 1 : 0;
}
