// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Look angles to a geostationary satellite from a point on the Earth: azimuth (degrees clockwise from true north) and elevation
// (degrees above the horizon, negative below it). Spherical Earth, no atmosphere, no refraction. No radio, no threads.
#pragma once

namespace dect2 {

struct GeoLookAngle {
    double azimuthDeg = 0;     // 0 north, 90 east, 180 south, 270 west, in [0, 360)
    double elevationDeg = 0;   // above the horizon; negative when the satellite is below it
};

// latUserDeg: user latitude (north positive); lonUserDeg: user longitude (east positive); lonSatDeg: satellite longitude (east positive).
GeoLookAngle geoLookAngle(double latUserDeg, double lonUserDeg, double lonSatDeg);

} // namespace dect2
