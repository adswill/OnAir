// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Geostationary look angles for three hand-computed cases (spherical Earth, GEO radius 42164 km, tolerance 0.1 degree):
// 1. user on the equator at 0 E, satellite at 10 E: due east, elevation atan((42164 cos10 - 6378.1) / (42164 sin10)) = 78.23.
// 2. user at 45 N, satellite on the same meridian (0 E): due south (180), elevation atan((cos45 - 0.15127) / sin45) = 38.17.
// 3. user on the equator at 10 E, satellite at 0 E: due west (270), elevation 78.23 again (symmetric).
#include "dect2/specan/geo_look.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static double angDiff(double a, double b) {
    double d = std::fabs(a - b);
    return d > 180.0 ? 360.0 - d : d;
}

int main() {
    const double tol = 0.1;
    GeoLookAngle a = geoLookAngle(0.0, 0.0, 10.0);
    CHECK(angDiff(a.azimuthDeg, 90.0) < tol, "case 1 azimuth %.3f, expected 90", a.azimuthDeg);
    CHECK(std::fabs(a.elevationDeg - 78.23) < tol, "case 1 elevation %.3f, expected 78.23", a.elevationDeg);

    GeoLookAngle b = geoLookAngle(45.0, 0.0, 0.0);
    CHECK(angDiff(b.azimuthDeg, 180.0) < tol, "case 2 azimuth %.3f, expected 180", b.azimuthDeg);
    CHECK(std::fabs(b.elevationDeg - 38.17) < tol, "case 2 elevation %.3f, expected 38.17", b.elevationDeg);

    GeoLookAngle c = geoLookAngle(0.0, 10.0, 0.0);
    CHECK(angDiff(c.azimuthDeg, 270.0) < tol, "case 3 azimuth %.3f, expected 270", c.azimuthDeg);
    CHECK(std::fabs(c.elevationDeg - 78.23) < tol, "case 3 elevation %.3f, expected 78.23", c.elevationDeg);

    return testSummary("specan_geo");
}
