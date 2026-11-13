// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Geometry for the position estimator: a local azimuthal-equidistant (ENU) frame, great-circle helpers, small fixed-size matrices.
// Spherical Earth (R = 6371008.8 m): the frame and its inverse are exact on the sphere, so the filter is self-consistent; against the
// WGS-84 ellipsoid distances differ by up to 0.3 %. Works over the date line (longitudes are wrapped) and at the poles.
#pragma once
#include <cmath>

namespace dect2::track {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kEarthR = 6371008.8;
constexpr double kKt = 1852.0 / 3600.0;       // m/s per knot
constexpr double kNm = 1852.0;
constexpr double kFt = 0.3048;
constexpr double kFpm = 0.3048 / 60.0;

inline double wrap180(double d) { d = std::fmod(d + 180.0, 360.0); if (d < 0) d += 360.0; return d - 180.0; }
inline double wrap360(double d) { d = std::fmod(d, 360.0); if (d < 0) d += 360.0; return d; }
inline double wrapPi(double r) { r = std::fmod(r + kPi, 2 * kPi); if (r < 0) r += 2 * kPi; return r - kPi; }

// Local tangent frame: x east, y north, metres, azimuthal equidistant about (lat0, lon0).
struct Frame {
    double lat0 = 0, lon0 = 0, sl0 = 0, cl0 = 1;
    void set(double lat, double lon) { lat0 = lat; lon0 = wrap180(lon); sl0 = std::sin(lat * kDeg); cl0 = std::cos(lat * kDeg); }
    void toLocal(double lat, double lon, double& e, double& n) const {
        const double dl = wrap180(lon - lon0) * kDeg, sp = std::sin(lat * kDeg), cp = std::cos(lat * kDeg);
        const double y = cp * std::sin(dl), x = cl0 * sp - sl0 * cp * std::cos(dl);
        const double s = std::hypot(x, y), cc = sl0 * sp + cl0 * cp * std::cos(dl);
        if (s < 1e-15) { e = n = 0; return; }
        const double c = std::atan2(s, cc), k = kEarthR * c / s;
        e = k * y; n = k * x;
    }
    void toGeo(double e, double n, double& lat, double& lon) const {
        const double rho = std::hypot(e, n);
        if (rho < 1e-9) { lat = lat0; lon = lon0; return; }
        const double c = rho / kEarthR, sc = std::sin(c), cc = std::cos(c), sa = e / rho, ca = n / rho;
        double sp = sl0 * cc + cl0 * sc * ca;
        sp = sp > 1 ? 1 : sp < -1 ? -1 : sp;
        lat = std::asin(sp) / kDeg;
        lon = wrap180(lon0 + std::atan2(sc * sa, cl0 * cc - sl0 * sc * ca) / kDeg);
    }
};

// Great circle on the sphere
inline double gcDistM(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * kDeg, p2 = lat2 * kDeg, dp = p2 - p1, dl = wrap180(lon2 - lon1) * kDeg;
    const double h = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * kEarthR * std::asin(std::fmin(1.0, std::sqrt(h)));
}
inline double gcBearingDeg(double lat1, double lon1, double lat2, double lon2) {   // initial bearing, 0..360
    const double p1 = lat1 * kDeg, p2 = lat2 * kDeg, dl = wrap180(lon2 - lon1) * kDeg;
    return wrap360(std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl)) / kDeg);
}
inline void gcDestination(double lat, double lon, double brgDeg, double distM, double& la, double& lo) {
    const double d = distM / kEarthR, b = brgDeg * kDeg, p = lat * kDeg;
    double sp = std::sin(p) * std::cos(d) + std::cos(p) * std::sin(d) * std::cos(b);
    sp = sp > 1 ? 1 : sp < -1 ? -1 : sp;
    la = std::asin(sp) / kDeg;
    lo = wrap180(lon + std::atan2(std::sin(b) * std::sin(d) * std::cos(p), std::cos(d) - std::sin(p) * sp) / kDeg);
}
// Cross-track (positive right of the path) and along-track distance of point P from the great circle A -> B, metres.
inline void gcTrackXy(double latA, double lonA, double latB, double lonB, double latP, double lonP, double& xt, double& at) {
    const double dAP = gcDistM(latA, lonA, latP, lonP) / kEarthR;
    const double bAP = gcBearingDeg(latA, lonA, latP, lonP) * kDeg, bAB = gcBearingDeg(latA, lonA, latB, lonB) * kDeg;
    double sx = std::sin(dAP) * std::sin(bAP - bAB);
    sx = sx > 1 ? 1 : sx < -1 ? -1 : sx;
    const double dxt = std::asin(sx);
    xt = dxt * kEarthR;
    const double ca = std::cos(dAP) / std::fmax(1e-15, std::cos(dxt));
    at = std::copysign(std::acos(ca > 1 ? 1 : ca < -1 ? -1 : ca), std::cos(bAP - bAB)) * kEarthR;
}

// Small fixed-size matrix (row major). Only what the filters use.
template <int R, int C> struct Mat {
    double a[R][C];
    Mat() { for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) a[i][j] = 0; }
    double& operator()(int i, int j) { return a[i][j]; }
    double operator()(int i, int j) const { return a[i][j]; }
    static Mat eye() { Mat m; for (int i = 0; i < (R < C ? R : C); i++) m.a[i][i] = 1; return m; }
};
template <int R, int K, int C> inline Mat<R, C> operator*(const Mat<R, K>& x, const Mat<K, C>& y) {
    Mat<R, C> o;
    for (int i = 0; i < R; i++) for (int k = 0; k < K; k++) { const double v = x.a[i][k]; if (v == 0) continue; for (int j = 0; j < C; j++) o.a[i][j] += v * y.a[k][j]; }
    return o;
}
template <int R, int C> inline Mat<C, R> tr(const Mat<R, C>& x) { Mat<C, R> o; for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) o.a[j][i] = x.a[i][j]; return o; }
template <int R, int C> inline Mat<R, C> operator+(const Mat<R, C>& x, const Mat<R, C>& y) { Mat<R, C> o; for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) o.a[i][j] = x.a[i][j] + y.a[i][j]; return o; }
template <int R, int C> inline Mat<R, C> operator-(const Mat<R, C>& x, const Mat<R, C>& y) { Mat<R, C> o; for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) o.a[i][j] = x.a[i][j] - y.a[i][j]; return o; }
template <int R, int C> inline Mat<R, C> operator*(double s, const Mat<R, C>& x) { Mat<R, C> o; for (int i = 0; i < R; i++) for (int j = 0; j < C; j++) o.a[i][j] = s * x.a[i][j]; return o; }
template <int N> inline void symmetrize(Mat<N, N>& p) { for (int i = 0; i < N; i++) for (int j = i + 1; j < N; j++) { const double v = 0.5 * (p.a[i][j] + p.a[j][i]); p.a[i][j] = p.a[j][i] = v; } }

}  // namespace dect2::track
