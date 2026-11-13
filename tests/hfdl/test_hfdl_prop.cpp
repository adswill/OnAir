// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HFDL propagation map (hfdl_prop.h): three simulated days of squitters (one per 32 s frame per frequency) from ground stations whose
// frequencies open and close by the hour with given probabilities, plus listening gaps; checks the measured rates, the best frequency
// per station and hour, the best frequency overall, the hours heard, the "station up" openings and the save / load round trip.
// Simulated only: no real multi-day HFDL log was used.
#include "dect2/hfdl/hfdl_prop.h"
#include <cmath>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

// the chance that a squitter of station gs on khz is decoded at UTC hour h
static double model(int gs, int khz, int h) {
    if (gs == 2 && khz == 8942) return h >= 6 && h < 18 ? 0.7 : 0;      // a daytime station on 8 MHz ...
    if (gs == 2 && khz == 11384) return h >= 10 && h < 16 ? 0.9 : 0;    // ... and better on 11 MHz around noon
    if (gs == 7 && khz == 5547) return h < 4 || h >= 20 ? 0.5 : 0;      // a night station on 5.5 MHz
    return 0;
}

int main() {
    const double t0 = 1767225600;   // 2026-01-01 00:00 UTC
    const int freqs[3] = {5547, 8942, 11384};
    for (unsigned seed = 1; seed <= 3; seed++) {
        std::mt19937 r(seed); std::uniform_real_distribution<double> u(0, 1);
        HfdlPropagation p;
        std::vector<HfdlPropagation::Up> ups;
        for (double t = t0; t < t0 + 3 * 86400; t += 32) {
            const int h = HfdlPropagation::hourOf(t);
            if (seed == 3 && h == 13) continue;   // the receiver was off at 13 UTC every day
            p.listened(t, {freqs[0], freqs[1], freqs[2]}, 32);
            for (int gs : {2, 7, 9})
                for (int k : freqs)
                    if (u(r) < model(gs, k, h)) p.heard(t + 1, gs, k, 10 + 3 * (float)u(r), true, &ups);
        }
        // rates
        double worst = 0;
        for (int gs : {2, 7})
            for (int k : freqs)
                for (int h = 0; h < 24; h++) {
                    const double rt = p.rate(gs, k, h);
                    if (seed == 3 && h == 13) { CHECK(rt < 0, "seed %u: hour 13 not listened yet has rate %.2f", seed, rt); continue; }
                    worst = std::max(worst, std::fabs(rt - model(gs, k, h)));
                }
        CHECK(worst < 0.12, "seed %u: rate error up to %.2f", seed, worst);
        CHECK(p.best(2, 12).khz == 11384, "seed %u: best for 2 at 12 UTC %d", seed, p.best(2, 12).khz);
        CHECK(p.best(2, 7).khz == 8942, "seed %u: best for 2 at 07 UTC %d", seed, p.best(2, 7).khz);
        CHECK(p.best(2, 2).khz == 0, "seed %u: 2 at 02 UTC %d", seed, p.best(2, 2).khz);
        CHECK(p.bestFrequency(1).khz == 5547 && p.bestFrequency(14).khz == 11384, "seed %u: best overall %d / %d", seed, p.bestFrequency(1).khz, p.bestFrequency(14).khz);
        const auto hh = p.hoursHeard(2);
        CHECK(hh.size() == (seed == 3 ? 11u : 12u) && hh.front() == 6 && hh.back() == 17, "seed %u: station 2 heard at %zu hours", seed, hh.size());
        // openings: each morning for 2 and each evening for 7, after the first day
        int up2 = 0, up7 = 0;
        for (const auto& x : ups) { if (x.gsId == 2) up2++; if (x.gsId == 7) up7++; CHECK(x.silentSec >= p.upAfterSec, "short silence %.0f", x.silentSec); }
        CHECK(up2 == 2 && up7 == 3, "seed %u: ups %d / %d", seed, up2, up7);
        CHECK(p.last24(7, t0 + 3 * 86400 - 1) != 0 && p.last24(9, t0 + 3 * 86400) == 0, "seed %u: last 24 h", seed);
        // save / load
        HfdlPropagation q;
        CHECK(q.load(p.save()), "load failed");
        CHECK(q.best(2, 12).khz == 11384 && std::fabs(q.rate(7, 5547, 22) - p.rate(7, 5547, 22)) < 1e-9 && q.lastHeard(2) == std::floor(p.lastHeard(2)),
              "seed %u: round trip differs", seed);
    }
    return testSummary("hfdl_prop");
}
