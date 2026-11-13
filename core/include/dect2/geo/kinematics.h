// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Derived kinematics: ground speed, track and vertical rate from two position fixes of the same aircraft (ACARS, VDL2 and HFDL reports
// carry no speed of their own). Pure maths, header only, no radio and no UI.
//   - horizontal distance on the WGS-84 ellipsoid (Vincenty inverse; haversine on the sphere where it does not converge)
//   - ground speed = horizontal distance / dt (knots); the climb is NOT part of it: the 3D (slant) figure is separate
//   - track = mean bearing of the chord (the great-circle bearing at its midpoint); over a long gap it is an average, flagged as such
//   - uncertainty: sqrt(2) * sigmaPos / dt plus the timing jitter; the position sigma comes from the rounding of the reports
//     (0.1 minute of arc = 185 m, 1 minute = 1.85 km, found from the coordinates themselves)
//   - time: the time inside the report (seconds of the UTC day) when both fixes have one, else the reception time and a wider jitter
// Never overwrites measured values: chooseKin() takes the measured value first, then the estimator's, then this.
#pragma once
#include <algorithm>
#include <cmath>

namespace dect2::geo {

constexpr double kKinPi = 3.14159265358979323846;
constexpr double kKinDeg = kKinPi / 180.0;
constexpr double kKinKt = 1852.0 / 3600.0;       // m/s per knot
constexpr double kKinFt = 0.3048;

// ---- WGS-84 inverse problem ----
struct Geodesic { double distM = 0, az1Deg = 0, az2Deg = 0; bool ellipsoid = true; };   // azimuths 0..360, az2 = forward azimuth at point 2

inline double kinWrap360(double d) { d = std::fmod(d, 360.0); return d < 0 ? d + 360.0 : d; }
inline double kinWrap180(double d) { d = std::fmod(d + 180.0, 360.0); if (d < 0) d += 360.0; return d - 180.0; }

inline Geodesic geodesicInverse(double lat1, double lon1, double lat2, double lon2) {
    constexpr double a = 6378137.0, f = 1 / 298.257223563, b = a * (1 - f);
    Geodesic g;
    const double L = kinWrap180(lon2 - lon1) * kKinDeg;
    const double U1 = std::atan((1 - f) * std::tan(lat1 * kKinDeg)), U2 = std::atan((1 - f) * std::tan(lat2 * kKinDeg));
    const double sU1 = std::sin(U1), cU1 = std::cos(U1), sU2 = std::sin(U2), cU2 = std::cos(U2);
    double lam = L, sinSig = 0, cosSig = 1, sig = 0, cos2a = 1, cos2sm = 0, sinLam = 0, cosLam = 1, sinAl = 0;
    bool conv = false;
    for (int i = 0; i < 100; i++) {
        sinLam = std::sin(lam); cosLam = std::cos(lam);
        sinSig = std::hypot(cU2 * sinLam, cU1 * sU2 - sU1 * cU2 * cosLam);
        if (sinSig < 1e-13) { g.distM = 0; return g; }   // same point
        cosSig = sU1 * sU2 + cU1 * cU2 * cosLam;
        sig = std::atan2(sinSig, cosSig);
        sinAl = cU1 * cU2 * sinLam / sinSig;
        cos2a = 1 - sinAl * sinAl;
        cos2sm = cos2a != 0 ? cosSig - 2 * sU1 * sU2 / cos2a : 0;
        const double C = f / 16 * cos2a * (4 + f * (4 - 3 * cos2a));
        const double lamPrev = lam;
        lam = L + (1 - C) * f * sinAl * (sig + C * sinSig * (cos2sm + C * cosSig * (-1 + 2 * cos2sm * cos2sm)));
        if (std::fabs(lam - lamPrev) < 1e-12) { conv = true; break; }
        if (std::fabs(lam) > kKinPi) break;
    }
    if (!conv) {   // nearly antipodal: spherical figures are good enough (such a pair is rejected anyway)
        constexpr double R = 6371008.8;
        const double p1 = lat1 * kKinDeg, p2 = lat2 * kKinDeg;
        const double h = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) + std::cos(p1) * std::cos(p2) * std::sin(L / 2) * std::sin(L / 2);
        g.distM = 2 * R * std::asin(std::min(1.0, std::sqrt(h)));
        g.az1Deg = kinWrap360(std::atan2(std::sin(L) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(L)) / kKinDeg);
        g.az2Deg = kinWrap360(std::atan2(std::sin(L) * std::cos(p1), -std::cos(p2) * std::sin(p1) + std::sin(p2) * std::cos(p1) * std::cos(L)) / kKinDeg);
        g.ellipsoid = false;
        return g;
    }
    const double uSq = cos2a * (a * a - b * b) / (b * b);
    const double A = 1 + uSq / 16384 * (4096 + uSq * (-768 + uSq * (320 - 175 * uSq)));
    const double B = uSq / 1024 * (256 + uSq * (-128 + uSq * (74 - 47 * uSq)));
    const double dSig = B * sinSig * (cos2sm + B / 4 * (cosSig * (-1 + 2 * cos2sm * cos2sm) - B / 6 * cos2sm * (-3 + 4 * sinSig * sinSig) * (-3 + 4 * cos2sm * cos2sm)));
    g.distM = b * A * (sig - dSig);
    g.az1Deg = kinWrap360(std::atan2(cU2 * sinLam, cU1 * sU2 - sU1 * cU2 * cosLam) / kKinDeg);
    g.az2Deg = kinWrap360(std::atan2(cU1 * sinLam, -sU1 * cU2 + cU1 * sU2 * cosLam) / kKinDeg);
    return g;
}

// ---- rounding of the reports ----
// Position resolution found from the coordinates: both multiples of 1 minute -> 1852 m, of 0.1 minute -> 185 m, else 30 m (ADS-C and
// decimal-degree reports). Metres.
inline double inferPosResM(double lat, double lon) {
    auto isMul = [](double v, double unit) { const double x = v / unit; return std::fabs(x - std::round(x)) < 2e-3; };
    if (isMul(lat, 1.0 / 60.0) && isMul(lon, 1.0 / 60.0)) return 1852.0;
    if (isMul(lat, 1.0 / 600.0) && isMul(lon, 1.0 / 600.0)) return 185.2;
    return 30.0;
}

// ---- the pure derivation ----
struct KinFix {
    double lat = 0, lon = 0;
    bool hasAlt = false; double altFt = 0;
    double t = 0;                  // reception time, seconds on any steady clock
    double repSecOfDay = -1;       // the time inside the report, seconds of the UTC day (-1: none)
    double posResM = -1;           // rounding of the position, metres (<= 0: inferred from the coordinates)
    double altResFt = 100;         // rounding of the altitude
};

struct KinOptions {
    double minDtSec = 60;          // a shorter gap says nothing
    double minDistSigmas = 3;      // the chord must be this many times its own error
    double longGapSec = 1200;      // from here the figures are averages over a possibly curved path
    double maxGapSec = 5400;       // beyond this no figure
    double maxKt = 1000;           // sanity cap on ground speed
    double maxVsFpm = 8000;
    double minTrackKt = 30;        // below this the bearing of a chord is noise
    double jitterReportSec = 3, jitterRecvSec = 20;
    double maxSigmaFrac = 0.5;     // reject when sigma exceeds this share of the speed
};

enum class KinReason : unsigned char { Ok = 0, BadInput, TooShort, TooLong, TooClose, TooFast, TooUncertain, ClockMismatch };

struct DerivedKin {
    bool ok = false;
    KinReason reason = KinReason::BadInput;
    double dtSec = 0;              // time between the two fixes
    bool usedReportTime = false;
    double distM = 0;              // horizontal chord on the ellipsoid
    double gsKt = 0, gsSigmaKt = 0;
    bool hasTrack = false; double trackDeg = 0, trackSigmaDeg = 0;
    bool hasVs = false; double vsFpm = 0, vsSigmaFpm = 0;
    double slantKt = 0, dist3dM = 0;   // including the height change (secondary figure)
    bool averaged = false;         // long gap: an average over the gap; the real path may have been longer and curved
    bool trackOk = false;          // good enough to turn an icon (no long gap, small angle error, moving)
    bool ellipsoid = true;
};

inline DerivedKin deriveKin(const KinFix& p0, const KinFix& p1, const KinOptions& o = KinOptions()) {
    DerivedKin r;
    auto okLat = [](double v) { return std::isfinite(v) && std::fabs(v) <= 90.0; };
    if (!okLat(p0.lat) || !okLat(p1.lat) || !std::isfinite(p0.lon) || !std::isfinite(p1.lon) || !std::isfinite(p0.t) || !std::isfinite(p1.t)) return r;
    // time: the report's own clock when both have one and it agrees with the reception order, else the reception clock
    double dt = p1.t - p0.t, jit = o.jitterRecvSec;
    if (p0.repSecOfDay >= 0 && p1.repSecOfDay >= 0) {
        double d = std::fmod(p1.repSecOfDay - p0.repSecOfDay, 86400.0);
        if (d < -43200.0) d += 86400.0; else if (d > 43200.0) d -= 86400.0;
        if (d > 0 && std::fabs(d - dt) <= std::max(600.0, 0.5 * std::fabs(dt))) { dt = d; jit = o.jitterReportSec; r.usedReportTime = true; }
        else if (d > 0 && dt > 0) { r.reason = KinReason::ClockMismatch; return r; }
    }
    r.dtSec = dt;
    if (!(dt > 0)) { r.reason = KinReason::BadInput; return r; }
    if (dt < o.minDtSec) { r.reason = KinReason::TooShort; return r; }
    if (dt > o.maxGapSec) { r.reason = KinReason::TooLong; return r; }
    const Geodesic g = geodesicInverse(p0.lat, p0.lon, p1.lat, p1.lon);
    r.ellipsoid = g.ellipsoid;
    r.distM = g.distM;
    const double res0 = p0.posResM > 0 ? p0.posResM : inferPosResM(p0.lat, p0.lon);
    const double res1 = p1.posResM > 0 ? p1.posResM : inferPosResM(p1.lat, p1.lon);
    const double s0 = std::max(res0 * 0.5, 15.0), s1 = std::max(res1 * 0.5, 15.0);
    const double sigD = std::sqrt(s0 * s0 + s1 * s1);                   // sqrt(2) * sigma for equal fixes
    if (g.distM < o.minDistSigmas * sigD) { r.reason = KinReason::TooClose; return r; }
    const double gs = g.distM / dt / kKinKt;
    if (gs > o.maxKt) { r.reason = KinReason::TooFast; return r; }
    const double sPos = sigD / dt / kKinKt, sTime = gs * std::sqrt(2.0) * jit / dt;
    r.gsKt = gs; r.gsSigmaKt = std::hypot(sPos, sTime);
    if (r.gsSigmaKt > o.maxSigmaFrac * gs) { r.reason = KinReason::TooUncertain; return r; }
    // mean bearing of the chord: the circular mean of the azimuths at both ends
    const double a1 = g.az1Deg * kKinDeg, a2 = g.az2Deg * kKinDeg;
    r.trackDeg = kinWrap360(std::atan2(std::sin(a1) + std::sin(a2), std::cos(a1) + std::cos(a2)) / kKinDeg);
    r.trackSigmaDeg = std::min(90.0, std::atan(sigD / g.distM) / kKinDeg);
    r.hasTrack = gs >= o.minTrackKt;
    r.averaged = dt >= o.longGapSec;
    r.trackOk = r.hasTrack && !r.averaged && r.trackSigmaDeg <= 25.0;
    if (p0.hasAlt && p1.hasAlt) {
        const double dh = (p1.altFt - p0.altFt) * kKinFt;
        const double vs = (p1.altFt - p0.altFt) / dt * 60.0;
        if (std::fabs(vs) <= o.maxVsFpm) {
            r.hasVs = true; r.vsFpm = vs;
            const double sa = std::hypot(p0.altResFt * 0.5, p1.altResFt * 0.5);
            r.vsSigmaFpm = std::hypot(sa / dt * 60.0, std::fabs(vs) * std::sqrt(2.0) * jit / dt);
            r.dist3dM = std::hypot(g.distM, dh);
            r.slantKt = r.dist3dM / dt / kKinKt;
        }
    }
    r.ok = true; r.reason = KinReason::Ok;
    return r;
}

// Newest pair out of a track (oldest first): walks back until the pair passes the rules, so a repeated or rounded-to-the-same-spot
// report does not hide an older usable one. `get(i)` -> KinFix of point i.
template <class GetFix>
inline DerivedKin deriveFromTrack(int n, GetFix get, const KinOptions& o = KinOptions()) {
    DerivedKin best;
    if (n < 2) return best;
    const KinFix last = get(n - 1);
    for (int i = n - 2; i >= 0 && i >= n - 8; i--) {
        DerivedKin d = deriveKin(get(i), last, o);
        if (d.ok) return d;
        if (d.reason != KinReason::TooShort && d.reason != KinReason::TooClose) { if (!best.dtSec) best = d; if (d.reason == KinReason::TooLong) break; }
        else if (!best.dtSec) best = d;
    }
    return best;
}

// ---- which value to show ----
enum class KinSource : unsigned char { None = 0, Measured, Estimated, Derived };
struct KinValue {
    KinSource src = KinSource::None;
    bool hasGs = false; double gsKt = 0, gsSigmaKt = 0;
    bool hasTrack = false; double trackDeg = 0; bool trackOk = false;
    bool hasVs = false; double vsFpm = 0;
    bool averaged = false; double dtSec = 0;   // derived only
};
struct KinMeasured { bool hasGs = false; double gsKt = 0; bool hasTrack = false; double trackDeg = 0; bool hasVs = false; double vsFpm = 0; };
struct KinEstimated { bool valid = false; double gsKt = 0, trackDeg = 0, vsFpm = 0; };

// Measured (broadcast or reported) first and never replaced; each missing field is filled from the estimator's filtered state, then from
// the two point value. A field's own source is the first that supplied the speed.
inline KinValue chooseKin(const KinMeasured& m, const KinEstimated& e, const DerivedKin& d) {
    KinValue v;
    if (m.hasGs) { v.hasGs = true; v.gsKt = m.gsKt; v.src = KinSource::Measured; }
    if (m.hasTrack) { v.hasTrack = true; v.trackDeg = m.trackDeg; v.trackOk = true; if (v.src == KinSource::None) v.src = KinSource::Measured; }
    if (m.hasVs) { v.hasVs = true; v.vsFpm = m.vsFpm; }
    if (e.valid) {
        if (!v.hasGs) { v.hasGs = true; v.gsKt = e.gsKt; if (v.src == KinSource::None) v.src = KinSource::Estimated; }
        if (!v.hasTrack && e.gsKt >= 30) { v.hasTrack = true; v.trackDeg = e.trackDeg; v.trackOk = true; if (v.src == KinSource::None) v.src = KinSource::Estimated; }
        if (!v.hasVs) { v.hasVs = true; v.vsFpm = e.vsFpm; }
    }
    if (d.ok) {
        const bool any = v.hasGs || v.hasTrack || v.hasVs;
        if (!v.hasGs) { v.hasGs = true; v.gsKt = d.gsKt; v.gsSigmaKt = d.gsSigmaKt; }
        if (!v.hasTrack && d.hasTrack) { v.hasTrack = true; v.trackDeg = d.trackDeg; v.trackOk = d.trackOk; }
        if (!v.hasVs && d.hasVs) { v.hasVs = true; v.vsFpm = d.vsFpm; }
        if (v.src == KinSource::None || !any) v.src = KinSource::Derived;
        v.averaged = d.averaged; v.dtSec = d.dtSec;
    }
    return v;
}

}  // namespace dect2::geo
