// Low-precision Sun position (see sun.h).
#include "dect2/smart5/sun.h"
#include <cmath>

namespace dect2 {
namespace smart5 {

namespace {
const double kPi = 3.14159265358979323846, kD = kPi / 180;
double norm360(double a) { a = std::fmod(a, 360); return a < 0 ? a + 360 : a; }
}

SunPos sunAt(double t) {
    SunPos s;
    const double n = t / 86400.0 + 2440587.5 - 2451545.0;   // days from J2000.0
    const double g = norm360(357.529 + 0.98560028 * n) * kD;
    const double q = norm360(280.459 + 0.98564736 * n);
    const double L = norm360(q + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * kD;
    const double e = (23.439 - 0.00000036 * n) * kD;
    s.raDeg = norm360(std::atan2(std::cos(e) * std::sin(L), std::cos(L)) / kD);
    s.decDeg = std::asin(std::sin(e) * std::sin(L)) / kD;
    s.gmstDeg = norm360((18.697374558 + 24.06570982441908 * n) * 15);
    const double ha = (s.raDeg - s.gmstDeg) * kD, dc = s.decDeg * kD;   // Earth-fixed longitude of the sub-solar point
    s.ecef[0] = std::cos(dc) * std::cos(ha);
    s.ecef[1] = std::cos(dc) * std::sin(ha);
    s.ecef[2] = std::sin(dc);
    return s;
}

void siteEcef(double lat, double lon, double alt, double o[3]) {
    const double r = 6371.0 + alt;
    o[0] = r * std::cos(lat * kD) * std::cos(lon * kD);
    o[1] = r * std::cos(lat * kD) * std::sin(lon * kD);
    o[2] = r * std::sin(lat * kD);
}

void dirAzEl(const double v[3], double lat, double lon, double& az, double& el) {
    const double la = lat * kD, lo = lon * kD;
    const double e = -std::sin(lo) * v[0] + std::cos(lo) * v[1];
    const double nn = -std::sin(la) * std::cos(lo) * v[0] - std::sin(la) * std::sin(lo) * v[1] + std::cos(la) * v[2];
    const double u = std::cos(la) * std::cos(lo) * v[0] + std::cos(la) * std::sin(lo) * v[1] + std::sin(la) * v[2];
    az = norm360(std::atan2(e, nn) / kD);
    el = std::atan2(u, std::sqrt(e * e + nn * nn)) / kD;
}

void sunAzEl(double t, double lat, double lon, double& az, double& el) {
    const SunPos s = sunAt(t);
    dirAzEl(s.ecef, lat, lon, az, el);
}

void radecAzEl(double t, double ra, double dec, double lat, double lon, double& az, double& el) {
    const SunPos s = sunAt(t);
    const double ha = (ra - s.gmstDeg) * kD, dc = dec * kD;
    const double v[3] = {std::cos(dc) * std::cos(ha), std::cos(dc) * std::sin(ha), std::sin(dc)};
    dirAzEl(v, lat, lon, az, el);
}

} // namespace smart5
} // namespace dect2
