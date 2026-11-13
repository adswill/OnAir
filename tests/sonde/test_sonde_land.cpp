// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radiosonde landing prediction: straight down lands below, drift moves the landing point, a rising sonde has no prediction.
#include "dect2/sonde/sonde_land.h"
#include <cmath>
#include <cstdio>
#include "../common/test_util.h"
int main() {
    using namespace dect2;
    auto a = predictSondeLanding(54.0, 25.0, 10000, -20, 0, 0);
    CHECK(a.valid && std::fabs(a.lat - 54.0) < 1e-6 && std::fabs(a.lon - 25.0) < 1e-6, "no drift lands below");
    CHECK(a.secondsLeft > 500 && a.secondsLeft < 2500, "time from 10 km at 20 m/s: %.0f s", a.secondsLeft);
    auto b = predictSondeLanding(54.0, 25.0, 10000, -20, 10, 90);
    CHECK(b.valid && b.lon > 25.0 && std::fabs(b.lat - 54.0) < 1e-3, "drift east: lon %.4f lat %.4f", b.lon, b.lat);
    const double kmE = (b.lon - 25.0) * 111.32 * std::cos(54.0 * 3.14159265 / 180);
    CHECK(kmE > 5 && kmE < 25, "10 m/s for %.0f s = %.1f km", b.secondsLeft, kmE);
    auto c = predictSondeLanding(54.0, 25.0, 10000, 5, 10, 90);
    CHECK(!c.valid, "rising: none");
    auto d = predictSondeLanding(54.0, 25.0, 5, -5, 10, 90);
    CHECK(!d.valid, "already down: none");
    if (!fails) printf("sonde landing ok\n");
    return fails ? 1 : 0;
}
