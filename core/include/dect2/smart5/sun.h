// Low-precision Sun position for the smart5 engines (sun outage, drift scan, terminator). The USNO / Astronomical Almanac
// approximation (mean anomaly g, mean longitude q, ecliptic longitude L = q + 1.915 sin g + 0.020 sin 2g), good to about 0.01 deg
// between 1950 and 2050: enough for beams of 0.3 deg or wider and for sunrise times to a few seconds.
#pragma once

namespace dect2 {
namespace smart5 {

struct SunPos { double raDeg = 0, decDeg = 0, gmstDeg = 0; double ecef[3] = {0, 0, 0}; };   // ecef: unit vector Earth-fixed

SunPos sunAt(double unixSec);
// topocentric azimuth (from north, clockwise) and elevation in degrees, refraction ignored
void sunAzEl(double unixSec, double latDeg, double lonDeg, double& azDeg, double& elDeg);
// azimuth / elevation of an Earth-fixed unit direction (or vector) seen from (lat, lon)
void dirAzEl(const double v[3], double latDeg, double lonDeg, double& azDeg, double& elDeg);
// azimuth / elevation of a celestial object (RA, dec in degrees) at a time
void radecAzEl(double unixSec, double raDeg, double decDeg, double latDeg, double lonDeg, double& azDeg, double& elDeg);
// observer position on a spherical Earth (km), Earth-fixed
void siteEcef(double latDeg, double lonDeg, double altKm, double out[3]);

} // namespace smart5
} // namespace dect2
