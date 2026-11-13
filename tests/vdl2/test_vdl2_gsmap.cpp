// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ground stations learned from the aircraft that talk to them: the estimate, coverage and how a taxiing aircraft pulls the estimate. Simulated positions.
#include "dect2/vdl2/vdl2_tel.h"
#include <random>
using namespace dect2;
#include "../common/test_util.h"
int main() {
    const double la = 54.63, lo = 25.28;
    std::mt19937 rng(3);
    for (double spread : {0.5, 1.0, 2.0}) {
        std::normal_distribution<double> n(0, spread);
        Vdl2GroundStation g;
        for (int i = 0; i < 300; i++) vdl2StationLearn(g, la + n(rng), lo + n(rng) / 0.58, false);
        CHECK(g.hasEstimate, "estimate");
        CHECK(vdl2DistNm(g.estLat, g.estLon, la, lo) < 20 * spread, "spread %.1f: off by %.1f nm", spread, vdl2DistNm(g.estLat, g.estLon, la, lo));
        CHECK(g.coverageNm > 30 * spread, "coverage %.0f", g.coverageNm);
    }
    {   // aircraft on the ground outweigh a lopsided airborne sample
        Vdl2GroundStation g;
        for (int i = 0; i < 40; i++) vdl2StationLearn(g, la + 1.5, lo, false);
        for (int i = 0; i < 20; i++) vdl2StationLearn(g, la, lo, true);
        CHECK(vdl2DistNm(g.estLat, g.estLon, la, lo) < 10, "ground wins: %.1f nm", vdl2DistNm(g.estLat, g.estLon, la, lo));
    }
    {   // too few fixes: no estimate; a known location is used for the coverage
        Vdl2GroundStation g; vdl2StationLearn(g, la, lo, false); vdl2StationLearn(g, la, lo, false);
        CHECK(!g.hasEstimate, "two fixes are not enough");
        Vdl2GroundStation k; k.hasPos = true; k.lat = la; k.lon = lo; vdl2StationLearn(k, la + 2, lo, false);
        CHECK(k.coverageNm > 110 && k.coverageNm < 130, "coverage from the known location %.0f", k.coverageNm);
    }
    { // across the antimeridian the estimate stays on the right side
        Vdl2GroundStation g; for (int i = 0; i < 10; i++) { vdl2StationLearn(g, 10, 179.9, false); vdl2StationLearn(g, 10, -179.9, false); }
        CHECK(std::fabs(g.estLon) > 179.5, "antimeridian %.2f", g.estLon);
    }
    return testSummary("vdl2_gsmap");
}
