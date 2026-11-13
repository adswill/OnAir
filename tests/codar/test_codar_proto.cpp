// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CODAR / HF ocean radar: the band table, the scan plan and the hint text (codar_rx.h).
#include "dect2/codar/codar_rx.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    int n = 0;
    const codar::Band* b = codar::bands(n);
    CHECK(n == 5 && b[0].loMhz < 4.45 && b[4].hiMhz > 42.5, "%d bands", n);
    CHECK(codar::bandOf(4.5) == "4.4 - 4.65 MHz" && codar::bandOf(13.5) == "12 - 14 MHz" && codar::bandOf(24.55) == "24.45 - 24.65 MHz" &&
          codar::bandOf(42.0) == "41.5 - 42.5 MHz" && codar::bandOf(5.25) == "5.25 MHz", "band names");
    CHECK(codar::bandOf(7.1).empty() && codar::bandOf(100).empty(), "outside the bands");
    for (double fs : {250e3, 1e6, 2e6, 8e6, 20e6}) {
        const auto plan = codar::scanPlan(fs);
        CHECK(!plan.empty(), "%.0f: empty plan", fs);
        // every point of every band is inside 0.4 x fs of a plan centre
        for (int i = 0; i < n; i++)
            for (double f = b[i].loMhz; f <= b[i].hiMhz; f += 0.005) {
                bool ok = false;
                for (double c : plan) if (std::fabs(f - c) <= 0.4 * fs / 1e6 + 1e-6) { ok = true; break; }
                if (!ok) { CHECK(false, "%.0f sps: %.3f MHz not covered", fs, f); break; }
            }
    }
    CHECK(codar::scanPlan(20e6).size() == 5, "a 20 Msps radio: one position per band (%zu)", codar::scanPlan(20e6).size());
    CHECK(codar::scanPlan(2e6).size() > 5 && codar::scanPlan(0).empty(), "plans");
    const std::string h = codar::hintFor(13.0, 50, 0.5);
    CHECK(h.find("possible") == 0 && h.find("SeaSonde") != std::string::npos && h.find("3.0 km") != std::string::npos, "hint '%s'", h.c_str());
    CHECK(codar::hintFor(0, 200, 0).find("possible HF ocean radar") == 0, "hint without period");
    printf(fails ? "codar proto: %d FAILED\n" : "codar proto: all passed\n", fails);
    return fails ? 1 : 0;
}
