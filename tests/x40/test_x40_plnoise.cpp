// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Power-line and impulse noise analyser: mains-related rates (100 and 120 Hz, 50 and 60 Hz), a drifting mains, missing and random extra impulses, other steady rates, random-only
// impulses (no pattern), noise alone, a level sweep. Simulated signals only.
#include "x40_gen2.h"
#include <cstring>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int patterns(const X40Telemetry& t) { int n = 0; for (auto& r : t.rows) if (r.kind == "Pattern" && r.id != "gone") n++; return n; }
static std::string firstPattern(const X40Telemetry& t) { std::string s; for (auto& r : t.rows) if (r.kind == "Pattern" && r.id != "gone") s = r.id + " | " + r.text; return s; }   // the oldest row is last
static double hz(const std::string& s) { return atof(s.c_str()); }

int main() {
    const double fs = 250000;
    struct C { double rate; const char* word; } cases[] = {{100, "twice the 50 Hz"}, {120, "twice the 60 Hz"}, {50, "half-wave"}, {60, "half-wave"}, {173, "switching supply"}};
    for (auto& c : cases) for (double fsx : {250000.0, 1000000.0}) {
        Pl p; p.rateHz = c.rate; auto t = run(x40::PlNoise, plNoise(p, fsx, 20.0), fsx);
        const std::string f = firstPattern(t);
        CHECK(patterns(t) == 1 && f.find(c.word) != std::string::npos && std::fabs(hz(f) - c.rate) < 0.05 * c.rate / 10 + 0.15, "rate %.0f fs %.0f: %d rows, '%s'", c.rate, fsx, patterns(t), f.c_str());
    }
    // a mains that drifts from 49.8 to 50.2 Hz (impulses at 99.6 - 100.4 Hz), 30 % of the impulses missing, 25 random extra impulses a second
    { Pl p; p.rateHz = 99.6; p.driftHzPerSec = 0.04; p.dropProb = 0.3; p.randomRate = 25; auto t = run(x40::PlNoise, plNoise(p, fs, 20.0), fs);
      CHECK(patterns(t) >= 1 && firstPattern(t).find("twice the 50 Hz") != std::string::npos, "drift: %s", firstPattern(t).c_str()); }
    // only random impulses (lightning, static): rate 100 per second Poisson, 20 s: no pattern
    { Pl p; p.rateHz = 0; p.randomRate = 100; p.randomDb = 14; auto t = run(x40::PlNoise, plNoise(p, fs, 25.0), fs);
      CHECK(patterns(t) == 0, "random impulses gave a pattern: %s", firstPattern(t).c_str()); CHECK(t.headline.find("without a repetition") != std::string::npos, "headline %s", t.headline.c_str()); }
    // noise alone, 15 minutes: no impulses, no pattern
    { int bad = 0; unsigned long long peaks = 0;
      for (int k = 0; k < 15; k++) { Pl p; p.rateHz = 0; p.seed = 40 + k; auto t = run(x40::PlNoise, plNoise(p, 96000, 60.0), 96000); bad += patterns(t); for (auto& f : t.facts) if (f.first == "impulses counted") peaks += strtoull(f.second.c_str(), nullptr, 10); }
      printf("noise alone, 15 minutes: %d false patterns, %llu impulses counted\n", bad, peaks); CHECK(bad == 0, "false patterns %d", bad); }
    // level sweep: peak of the impulses over the mean noise envelope
    printf("impulse level sweep (100 Hz, 20 %% missing, 70 us wide): peak over the wideband noise mean; the level read is against the 10 kHz analysis band:\n");
    for (double db : {25.0, 18.0, 14.0, 10.0, 6.0, 3.0, 0.0, -3.0}) {   // dB over the mean envelope of the wideband noise; the analysis band is 12 dB narrower
        int ok = 0; double lvl = 0;
        for (unsigned s = 1; s <= 4; s++) { Pl p; p.peakDb = db; p.dropProb = 0.2; p.seed = s; auto t = run(x40::PlNoise, plNoise(p, fs, 25.0), fs); if (patterns(t) >= 1) { ok++; } for (auto& f : t.facts) if (f.first == "impulse level") lvl += atof(f.second.c_str()) / 4; }
        printf("  %.0f dB: %d of 4, level read %.1f dB\n", db, ok, lvl);
        if (db >= 14) CHECK(ok == 4, "level %.0f dB: %d of 4", db, ok);
    }
    printf(fails ? "x40 plnoise: %d FAILED\n" : "x40 plnoise: ok\n", fails);
    return fails ? 1 : 0;
}
