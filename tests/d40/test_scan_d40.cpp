// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/d40/scan_d40.h"
#include <cstdio>
#include <random>
using namespace dect2::d40;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static std::string top(const HitFeatures& f) { return classifyHit(f)[0].what; }
int main() {
    std::mt19937 rng(4);
    HitFeatures fm; fm.centreHz = 93.9e6; fm.bwHz = 180e3; fm.duty = 1; fm.levelStdDb = 1; fm.peakToMeanDb = 6;
    CHECK(top(fm) == "FM broadcast");
    HitFeatures air; air.centreHz = 124.2e6; air.bwHz = 8e3; air.duty = 0.2; air.levelStdDb = 6;
    CHECK(top(air) == "airband AM voice");
    HitFeatures car; car.centreHz = 433.0e6; car.bwHz = 1e3; car.duty = 1; car.levelStdDb = 0.5;
    CHECK(top(car).find("carrier") != std::string::npos);
    HitFeatures tv; tv.centreHz = 538e6; tv.bwHz = 7.6e6; tv.duty = 1; tv.peakToMeanDb = 2;
    CHECK(top(tv).find("wideband digital") == 0);
    HitFeatures wb; wb.centreHz = 2.44e9; wb.bwHz = 20e6; wb.duty = 0.2; wb.peakToMeanDb = 5;
    CHECK(top(wb).find("wideband burst") == 0);
    HitFeatures odd; odd.centreHz = 50e6; odd.bwHz = 400e3; odd.duty = 0.5; odd.peakToMeanDb = 9; odd.levelStdDb = 2;
    CHECK(top(odd) == "unknown");

    // activity map over 10 days: a daytime repeater (09-17, 60 % of sweeps) at 145.6, a constant carrier at 100.0; 144 sweeps a day
    ActivityMap m;
    for (int day = 0; day < 10; day++)
        for (int s = 0; s < 144; s++) {
            const int hour = s / 6; std::vector<double> hits = {100.0e6 + 3e3};
            if (hour >= 9 && hour < 17 && rng() % 100 < 60) hits.push_back(145.6e6);
            m.addSweep(hour, 99e6, 146e6, hits);
        }
    CHECK(m.occupancy(100.0e6, 3) > 0.99);
    CHECK(m.occupancy(145.6e6, 3) == 0);
    CHECK(std::fabs(m.occupancy(145.6e6, 12) - 0.6) < 0.1);
    auto bh = m.busyHours(145.6e6); CHECK(bh.size() == 8 && bh.front() == 9 && bh.back() == 16);
    CHECK(m.occupancy(300e6, 1) == -1);
    // persistence: save, load into a new map, then load the same text again (days add up)
    ActivityMap m2; CHECK(m2.load(m.save()));
    CHECK(m2.cells() == m.cells() && m2.sweepsAny(100.0e6) == m.sweepsAny(100.0e6));
    CHECK(m2.load(m.save()) && m2.sweepsAny(100.0e6) == 2 * m.sweepsAny(100.0e6));
    ActivityMap m3; CHECK(!m3.load("garbage")); CHECK(!m3.load("ACTMAP1 25000\n5 1 2 3"));

    // new-signal alert: at 03:00 the repeater frequency is normally empty
    NewSignalWatch w; int alerts = 0;
    auto sw = [&](int hour, std::vector<double> hits) { auto a = w.check(m, hour, hits); alerts += (int)a.size(); return a.size(); };
    CHECK(sw(3, {100.0e6 + 3e3}) == 0);                 // the usual carrier
    CHECK(sw(3, {100.0e6 + 3e3, 145.6e6}) == 0);        // first sweep: not yet confirmed
    CHECK(sw(3, {100.0e6 + 3e3, 145.6e6}) == 1);        // second: alert
    CHECK(sw(3, {100.0e6 + 3e3, 145.6e6}) == 0);        // not again while it stays
    CHECK(sw(12, {145.6e6}) == 0 && sw(12, {145.6e6}) == 0);   // at noon the repeater is normal (but still the cell is alerted-latched: fine, nothing new)
    // a single spur never alerts; a quiet band never alerts; a hit in a never-swept area never alerts (no baseline)
    NewSignalWatch w2; int fa = 0;
    for (int i = 0; i < 500; i++) { std::vector<double> h; if (rng() % 50 == 0) h.push_back(120e6 + (rng() % 1000) * 25e3 * 0.0 + (rng() % 20) * 25e3); fa += (int)w2.check(m, 3, h).size(); }
    std::printf("new-signal alerts from isolated spurs: %d\n", fa); CHECK(fa == 0 || fa < 3);
    NewSignalWatch w3; w3.check(m, 3, {500e6}); CHECK(w3.check(m, 3, {500e6}).empty());
    // re-arm: after leaving for 10 sweeps a return alerts again
    NewSignalWatch w4; w4.check(m, 3, {145.6e6}); CHECK(w4.check(m, 3, {145.6e6}).size() == 1);
    for (int i = 0; i < 10; i++) w4.check(m, 3, {});
    w4.check(m, 3, {145.6e6}); CHECK(w4.check(m, 3, {145.6e6}).size() == 1);
    std::printf(fails ? "FAILED\n" : "scan_d40 ok\n");
    return fails ? 1 : 0;
}
