// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/d40/sat_d40.h"
#include <cstdio>
#include <random>
using namespace dect2::d40;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
// a transponder = raised-cosine-ish shaped plateau of bandwidth sr*1.35 plus chi-square noise; returns the power in dB per bin
static std::vector<float> makeSpan(const std::vector<std::pair<double, double>>& tps /*centre MHz, sr MHz*/, const std::vector<double>& cn, double lo, double binHz, int n, std::mt19937& rng) {
    std::vector<double> p((size_t)n, 1.0);
    for (size_t t = 0; t < tps.size(); t++) {
        const double bw = tps[t].second * 1.35;
        for (int i = 0; i < n; i++) {
            const double f = (lo + i * binHz) / 1e6 - tps[t].first;
            const double edge = std::fabs(f) - bw / 2;
            const double sh = edge < -0.175 * tps[t].second ? 1.0 : (edge > 0.175 * tps[t].second ? 0.0 : 0.5 * (1 - std::sin(M_PI / 2 * edge / (0.175 * tps[t].second))));
            p[(size_t)i] += sh * std::pow(10.0, cn[t] / 10.0);
        }
    }
    std::vector<float> db((size_t)n); std::gamma_distribution<double> gd(16, 1.0 / 16);   // 16-bin averaged periodogram
    for (int i = 0; i < n; i++) db[(size_t)i] = (float)(10 * std::log10(p[(size_t)i] * gd(rng)));
    return db;
}
int main() {
    std::mt19937 rng(8);
    const double binHz = 100e3; const int n = 1000; const double lo = 950e6;   // 950 - 1050 MHz... span 100 MHz
    for (int trial = 0; trial < 5; trial++) {
        std::vector<std::pair<double, double>> tps = {{958.0, 10.0}, {982.0, 20.0}, {1012.0, 12.0}, {1035.0, 6.0}};
        std::vector<double> cn = {12, 9, 14, 8};
        auto db = makeSpan(tps, cn, lo, binHz, n, rng);
        auto t = blindScan(db, binHz, lo);
        CHECK(t.size() == 4);
        if (t.size() == 4) for (size_t k = 0; k < 4; k++) {
            CHECK(std::fabs(t[k].centreHz / 1e6 - tps[k].first) < 0.5);
            CHECK(std::fabs(t[k].symbolRateHz / 1e6 - tps[k].second) < 0.05 * tps[k].second);
            CHECK(std::fabs(t[k].cnDb - cn[k]) < 2.0);
        }
    }
    // noise only: nothing
    { int fa = 0; for (int k = 0; k < 50; k++) { auto db = makeSpan({}, {}, lo, binHz, n, rng); fa += (int)blindScan(db, binHz, lo).size(); } std::printf("blind scan false transponders in 50 noise spans: %d\n", fa); CHECK(fa == 0); }
    // two touching carriers 12 + 12 Msym at 20 MHz spacing... (gap 3.8 MHz) are two
    { auto db = makeSpan({{995.0, 12.0}, {1013.0, 12.0}}, {12, 12}, lo, binHz, n, rng); CHECK(blindScan(db, binHz, lo).size() == 2); }
    CHECK(snapSymbolRate(27.3e6) == 27.5e6);
    CHECK(snapSymbolRate(26.0e6) == 26.0e6);
    // modcod: 7 dB Es/N0: 8PSK 2/3 needs 7.12 with the margin (not), 8PSK 3/5 and QPSK 9/10 tie in efficiency, the lower requirement wins
    { auto m = modcodFor(7.0); CHECK(!m.empty() && m[0].name == "8PSK 3/5" && m[0].efficiency == 1.8); auto m2 = modcodFor(7.2); CHECK(m2[0].name == "8PSK 2/3"); CHECK(modcodFor(-5).empty()); CHECK(modcodFor(20)[0].name == "32APSK 9/10"); }
    // pointing aid: sweep through a lobe
    { PointingAid p; double rate = 0, rateAtPeak = 0; int beforePeakTrend = 0, afterTrend = 0;
      for (int i = -20; i <= 20; i++) {
          const double cn = 14 - 0.03 * i * i + 0.1 * std::sin(i * 3.0);    // lobe, peak 14 at i = 0
          rate = p.feed(cn);
          if (i == -5) beforePeakTrend = p.trend();
          if (i == 0) rateAtPeak = rate;
          if (i == 8) afterTrend = p.trend();
      }
      CHECK(beforePeakTrend == 1 && afterTrend == -1);
      CHECK(rateAtPeak > 19 && rate < 12 && p.belowPeakDb() > 8);
      p.reset(); CHECK(p.feed(0.2) == 0); }
    // LNB drift: warm-up exponential 60 kHz -> 0 with 1-min time constant, plus measurement noise
    { LnbDrift d; std::normal_distribution<double> g(0, 2e3); const double lo = 9.75e9; bool settledEarly = false, ok = false;
      for (int s = 0; s < 900; s += 5) {
          const double err = 60e3 * std::exp(-s / 60.0) + g(rng);
          d.add(s, 1000e6, 1000e6 + err, lo);
          if (s == 60) { settledEarly = d.settled(); }
      }
      CHECK(!settledEarly);
      CHECK(d.settled(120, 5));
      CHECK(std::fabs(d.errorKhz()) < 10 && std::fabs(d.errorPpm()) < 1.5);
      d.driftKhzPerMin(120, &ok); CHECK(ok);
      // linear drift of +30 kHz/min is measured
      LnbDrift e; for (int s = 0; s < 300; s += 5) e.add(s, 1e9, 1e9 + 500.0 * s + g(rng), lo);
      CHECK(std::fabs(e.driftKhzPerMin(240) - 30.0) < 1.5); }
    std::printf(fails ? "FAILED\n" : "sat_d40 ok\n");
    return fails ? 1 : 0;
}
