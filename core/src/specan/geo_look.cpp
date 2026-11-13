// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/specan/geo_look.h"
#include <cmath>

namespace dect2 {

namespace {
const double kPi = 3.14159265358979323846;
const double kEarthRadiusKm = 6378.137;
const double kGeoRadiusKm = 42164.0;   // geostationary orbit radius from the Earth's centre
}

GeoLookAngle geoLookAngle(double latUserDeg, double lonUserDeg, double lonSatDeg) {
    const double d2r = kPi / 180.0;
    const double phi = latUserDeg * d2r, lam = lonUserDeg * d2r;
    // Earth-centred vectors (km): user on the sphere, satellite on the equator at the GEO radius.
    const double sp = std::sin(phi), cp = std::cos(phi), sl = std::sin(lam), cl = std::cos(lam);
    const double ux = kEarthRadiusKm * cp * cl, uy = kEarthRadiusKm * cp * sl, uz = kEarthRadiusKm * sp;
    const double ls = lonSatDeg * d2r;
    const double sx = kGeoRadiusKm * std::cos(ls), sy = kGeoRadiusKm * std::sin(ls);
    const double dx = sx - ux, dy = sy - uy, dz = -uz;
    // Local east, north and up at the user.
    const double e = -sl * dx + cl * dy;
    const double n = -sp * cl * dx - sp * sl * dy + cp * dz;
    const double u = cp * cl * dx + cp * sl * dy + sp * dz;
    GeoLookAngle r;
    r.azimuthDeg = std::atan2(e, n) / d2r;
    if (r.azimuthDeg < 0) r.azimuthDeg += 360.0;
    if (r.azimuthDeg >= 360.0) r.azimuthDeg -= 360.0;
    r.elevationDeg = std::atan2(u, std::sqrt(e * e + n * n)) / d2r;
    return r;
}

} // namespace dect2
