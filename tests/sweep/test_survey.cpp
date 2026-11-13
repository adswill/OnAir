// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Band survey: synthetic sweeps with an always-on carrier, a burst that comes and goes, and a quiet gap; diff between two surveys.
#include "../common/tmp_path.h"
#include "dect2/common/stable_random.h"
#include "dect2/sweep/survey.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2::sweep;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static Sweep make(double t, bool burst, double carrierHz, std::mt19937& rng, double floorOff = 0) {
    Sweep s; s.startHz = 100e6; s.binHz = 10e3; s.timeSec = t;
    const size_t n = 1000;   // 100.00 .. 109.99 MHz
    dect2::normal_distribution<float> g(0.f, 0.8f);
    s.db.resize(n); s.floorDb.assign(n, (float)(-90 + floorOff));
    for (size_t i = 0; i < n; i++) s.db[i] = (float)(-90 + floorOff) + g(rng);
    auto put = [&](double hz, float up) { const size_t i = (size_t)std::llround((hz - s.startHz) / s.binHz); for (int k = -1; k <= 1; k++) s.db[i + k] = (float)(-90 + floorOff) + up; };
    put(carrierHz, 30);
    if (burst) put(105.5e6, 25);
    s.floorOverallDb = (float)(-90 + floorOff);
    return s;
}

int main() {
    std::mt19937 rng(7);
    Survey a;
    for (int r = 0; r < 100; r++) CHECK(a.add(make(1e9 + r * 60, r % 4 == 0, 102.0e6, rng)), "add");
    CHECK(a.rows() == 100 && a.bins() == 1000, "rows %zu bins %zu", a.rows(), a.bins());
    auto p = a.persistent(0.5);
    CHECK(p.size() == 1 && std::fabs(p[0].freqHz - 102.0e6) < 15e3 && p[0].occupancy > 0.95f, "persistent %zu", p.size());
    CHECK(!p.empty() && p[0].bwHz >= 25e3 && p[0].bwHz <= 40e3, "bw %.0f", p.empty() ? 0.0 : p[0].bwHz);
    auto p2 = a.persistent(0.2);
    CHECK(p2.size() == 2, "burst at 25%% found with a lower limit: %zu", p2.size());
    for (const auto& x : p2) if (std::fabs(x.freqHz - 105.5e6) < 15e3) CHECK(std::fabs(x.occupancy - 0.25f) < 0.03f, "burst occupancy %.2f", x.occupancy);
    auto q = a.quiet(500e3, 3);
    CHECK(q.size() == 3, "quiet count %zu", q.size());
    for (const auto& s : q) {
        CHECK(!(s.startHz < 102.0e6 + 15e3 && s.startHz + s.widthHz > 102.0e6 - 15e3), "quiet slot overlaps the carrier");
        CHECK(!(s.startHz < 105.5e6 + 15e3 && s.startHz + s.widthHz > 105.5e6 - 15e3), "quiet slot overlaps the burst");
    }
    // the diff: a new carrier appears, the old one vanishes
    Survey b;
    for (int r = 0; r < 50; r++) b.add(make(2e9 + r * 60, false, 107.0e6, rng, 3.0));
    auto d = compareSurveys(a, b);
    CHECK(d.appeared.size() == 1 && std::fabs(d.appeared[0].freqHz - 107.0e6) < 15e3, "appeared %zu", d.appeared.size());
    CHECK(d.vanished.size() == 1 && std::fabs(d.vanished[0].freqHz - 102.0e6) < 15e3, "vanished %zu", d.vanished.size());
    CHECK(std::fabs(d.floorChangeDb - 3.0f) < 0.1f, "floor change %.2f", d.floorChangeDb);
    // save / load round trip
    std::string err;
    const std::string f = tmpPath("test_survey.bin");
    CHECK(a.save(f, err), "save %s", err.c_str());
    Survey c; CHECK(c.load(f, err), "load %s", err.c_str());
    CHECK(c.rows() == a.rows() && c.bins() == a.bins() && c.persistent(0.5).size() == 1 && c.lastTime() == a.lastTime(), "round trip");
    remove(f.c_str());
    CHECK(!c.load(tmpPath("does_not_exist_survey"), err), "missing file");
    // a sweep of a coarser grid lands on the same bins; one outside the range is refused
    Sweep coarse = make(3e9, false, 102.0e6, rng); coarse.binHz = 20e3; coarse.db.resize(500); coarse.floorDb.resize(500);
    CHECK(a.add(coarse), "coarse sweep added");
    Sweep farBy = coarse; farBy.startHz = 500e6;
    CHECK(!a.add(farBy), "out-of-range sweep refused");
    // the row cap merges the old rows and keeps the carrier
    SurveyConfig sc; sc.maxRows = 40;
    Survey m(sc);
    for (int r = 0; r < 200; r++) m.add(make(r, false, 102.0e6, rng));
    CHECK(m.rows() <= 40 && m.persistent(0.5).size() == 1, "cap: rows %zu", m.rows());
    auto img = a.heatmap(200, 50);
    CHECK(img.size() == 200 * 50 * 4 && img[3] == 255, "heatmap");
    CHECK(a.exportOccupancyCsv(tmpPath("test_survey.csv"), err) && a.exportSignalsCsv(tmpPath("test_survey_sig.csv"), 0.5, err), "csv");
    remove(tmpPath("test_survey.csv")); remove(tmpPath("test_survey_sig.csv"));
    printf(fails ? "survey: %d FAILED\n" : "survey: all ok\n", fails);
    {   // a signal that comes back regularly: one sweep every 300 s, on for one sweep every hour (with a jitter of a sweep); an irregular burster and a carrier that is always on are not listed
        Survey sv; std::mt19937 r2(5);
        std::uniform_real_distribution<double> u(0, 1);
        const double t0 = 1.7e9;
        for (int k = 0; k < 240; k++) {
            Sweep s = make(t0 + k * 300.0, false, 102e6, r2);
            auto put = [&](double hz, float up) { const size_t i = (size_t)std::llround((hz - s.startHz) / s.binHz); for (int j = -1; j <= 1; j++) s.db[i + j] += up; };
            if (k % 12 == 0 || (k % 12 == 1 && u(r2) < 0.15)) put(107e6, 22);       // hourly
            if (k > 30 && (k * k % 97) < 3) put(108e6, 22);                          // irregular
            sv.add(s);
        }
        const auto rec = sv.recurring();
        bool hourly = false; int others = 0;
        for (const auto& q : rec) { if (std::fabs(q.freqHz - 107e6) < 30e3) { hourly = true; CHECK(std::fabs(q.periodSec - 3600) <= 300, "period %.0f", q.periodSec); CHECK(q.nextSec >= t0 + 239 * 300.0, "next is in the future"); CHECK(q.starts >= 18, "starts %d", q.starts); } else others++; }
        CHECK(hourly, "hourly signal found");
        CHECK(others == 0, "other signals listed as recurring: %d", others);
    }
    return fails ? 1 : 0;
}
