// WGS84 helpers for the radiosonde receiver and its test signal: geodetic <-> ECEF, ECEF velocity <-> east / north / up, GPS time.
#pragma once
#include <cmath>
#include <cstdint>

namespace dect2 {
namespace sondegeo {

constexpr double kA = 6378137.0, kF = 1.0 / 298.257223563;
constexpr double kE2 = kF * (2 - kF);
constexpr double kPi = 3.14159265358979323846;
// GPS time runs ahead of UTC by the leap seconds since 1980 (18 since 2017-01-01). A sonde that sends GPS time is shown in UTC.
constexpr int kGpsMinusUtcS = 18;
constexpr double kGpsEpochUnix = 315964800.0;     // 1980-01-06 00:00:00 UTC

inline void llaToEcef(double latDeg, double lonDeg, double h, double& x, double& y, double& z) {
    const double la = latDeg * kPi / 180, lo = lonDeg * kPi / 180;
    const double n = kA / std::sqrt(1 - kE2 * std::sin(la) * std::sin(la));
    x = (n + h) * std::cos(la) * std::cos(lo);
    y = (n + h) * std::cos(la) * std::sin(lo);
    z = (n * (1 - kE2) + h) * std::sin(la);
}

inline void ecefToLla(double x, double y, double z, double& latDeg, double& lonDeg, double& h) {
    const double p = std::hypot(x, y);
    double la = std::atan2(z, p * (1 - kE2));
    double n = kA;
    h = 0;
    for (int i = 0; i < 8; i++) {
        n = kA / std::sqrt(1 - kE2 * std::sin(la) * std::sin(la));
        h = p / std::cos(la) - n;
        la = std::atan2(z, p * (1 - kE2 * n / (n + h)));
    }
    latDeg = la * 180 / kPi;
    lonDeg = std::atan2(y, x) * 180 / kPi;
}

// velocity (m/s) in ECEF at a position -> east, north, up
inline void ecefVelToEnu(double latDeg, double lonDeg, double vx, double vy, double vz, double& e, double& n, double& u) {
    const double la = latDeg * kPi / 180, lo = lonDeg * kPi / 180;
    e = -std::sin(lo) * vx + std::cos(lo) * vy;
    n = -std::sin(la) * std::cos(lo) * vx - std::sin(la) * std::sin(lo) * vy + std::cos(la) * vz;
    u = std::cos(la) * std::cos(lo) * vx + std::cos(la) * std::sin(lo) * vy + std::sin(la) * vz;
}

inline void enuVelToEcef(double latDeg, double lonDeg, double e, double n, double u, double& vx, double& vy, double& vz) {
    const double la = latDeg * kPi / 180, lo = lonDeg * kPi / 180;
    vx = -std::sin(lo) * e - std::sin(la) * std::cos(lo) * n + std::cos(la) * std::cos(lo) * u;
    vy = std::cos(lo) * e - std::sin(la) * std::sin(lo) * n + std::cos(la) * std::sin(lo) * u;
    vz = std::cos(la) * n + std::sin(la) * u;
}

inline double gpsToUnix(int week, double towS) { return kGpsEpochUnix + 604800.0 * week + towS - kGpsMinusUtcS; }

} // namespace sondegeo
} // namespace dect2
