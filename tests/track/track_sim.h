// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Trajectory and sensor simulator for the position estimator tests: flights between real city pairs (great-circle legs with lateral
// waypoints, bank-limited fly-by turns, climb and descent, speed wander), holding patterns, and per-source sensor noise.
// Deterministic (own generator and Box-Muller: the standard library's distributions differ between platforms).
#pragma once
#include "dect2/track/track_geo.h"
#include "dect2/track/track_imm.h"
#include "dect2/track/track_route.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace tsim {
using namespace dect2::track;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 2685821657736338717ull + 1442695040888963407ull) { for (int i = 0; i < 4; i++) next(); }
    uint64_t next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 2685821657736338717ull; }
    double u() { return ((next() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
    double n() { const double a = u(), b = u(); return std::sqrt(-2 * std::log(a)) * std::cos(2 * kPi * b); }
};

struct TState { double lat, lon, altM, trackDeg, speedMs, vrateMs, turnDegS; };

struct Truth {
    double t0 = 0, dt = 1;
    std::vector<TState> s;
    double end() const { return t0 + dt * (double)(s.size() - 1); }
    TState at(double t) const {
        double x = (t - t0) / dt;
        if (x < 0) x = 0;
        size_t i = (size_t)x;
        if (i + 1 >= s.size()) return s.back();
        const double f = x - (double)i;
        const TState &a = s[i], &b = s[i + 1];
        TState o = a;
        o.lat = a.lat + f * (b.lat - a.lat);
        o.lon = a.lon + f * wrap180(b.lon - a.lon);
        o.altM = a.altM + f * (b.altM - a.altM);
        o.speedMs = a.speedMs + f * (b.speedMs - a.speedMs);
        o.trackDeg = wrap360(a.trackDeg + f * wrap180(b.trackDeg - a.trackDeg));
        return o;
    }
};

inline void stepPos(TState& st, double dt) {
    double la, lo;
    gcDestination(st.lat, st.lon, st.trackDeg, st.speedMs * dt, la, lo);
    st.lat = la; st.lon = lo;
}

// A flight from (la1,lo1) to (la2,lo2): lateral waypoints up to `lateralKm` off the great circle, fly-by turns limited to 25 degrees of bank
inline Truth makeFlight(const Airport& A, const Airport& B, uint64_t seed, double cruiseKt = 470, double lateralKm = 70) {
    Rng r(seed);
    Truth tr; tr.dt = 1;
    const double total = gcDistM(A.lat, A.lon, B.lat, B.lon);
    std::vector<std::pair<double, double>> wp;
    const int nw = total > 1.2e6 ? 3 : 1;
    for (int i = 1; i <= nw; i++) {
        const double frac = (double)i / (nw + 1) + 0.04 * r.n();
        double la, lo;
        gcDestination(A.lat, A.lon, gcBearingDeg(A.lat, A.lon, B.lat, B.lon), total * frac, la, lo);
        const double off = lateralKm * 1000 * (r.u() * 2 - 1);
        double la2, lo2;
        gcDestination(la, lo, gcBearingDeg(A.lat, A.lon, B.lat, B.lon) + 90, off, la2, lo2);
        wp.push_back({la2, lo2});
    }
    wp.push_back({B.lat, B.lon});
    TState st{A.lat, A.lon, 20, gcBearingDeg(A.lat, A.lon, wp[0].first, wp[0].second), 80, 0, 0};
    const double cruiseAlt = 10500, cruiseV = cruiseKt * kKt;
    double omega = 0, vWander = 0;
    size_t k = 0;
    for (int step = 0; step < 14 * 3600; step++) {
        tr.s.push_back(st);
        const double dRem = gcDistM(st.lat, st.lon, B.lat, B.lon);
        if (dRem < 3500) break;
        // targets
        double alt = cruiseAlt, v = cruiseV + vWander;
        double vs = 0;
        const double climbDone = (cruiseAlt - st.altM);
        if (step < 60) { v = 90; }
        if (dRem < 150 * kNm) alt = std::max(0.0, dRem * 0.055 - 150.0);   // 3 degree slope to the runway
        else if (step > 60 && climbDone > 0) { v = std::min(v, 140 + st.altM * 0.032); }
        if (dRem < 150 * kNm) v = std::min(v, 90 + dRem / 1852.0 * 2.0 + 0.0);
        vs = std::clamp((alt - st.altM) * 0.05, -9.0, step < 60 ? 0.0 : 12.0);
        if (step < 60) vs = 0;
        // lateral guidance with fly-by
        double brg = gcBearingDeg(st.lat, st.lon, wp[k].first, wp[k].second);
        const double dw = gcDistM(st.lat, st.lon, wp[k].first, wp[k].second);
        if (k + 1 < wp.size()) {
            const double next = gcBearingDeg(wp[k].first, wp[k].second, wp[k + 1].first, wp[k + 1].second);
            const double dpsi = std::fabs(wrap180(next - brg)) * kDeg;
            const double R = st.speedMs * st.speedMs / (9.80665 * std::tan(25 * kDeg));
            if (dw < R * std::tan(dpsi / 2) + 500) { k++; brg = next; }
        }
        const double err = wrap180(brg - st.trackDeg);
        const double wmax = 9.80665 * std::tan(25 * kDeg) / std::max(60.0, st.speedMs);   // rad/s
        double wcmd = std::clamp(err * kDeg * 0.15, -wmax, wmax);
        const double rate = 0.5 * kDeg;                            // roll-in: turn rate changes by at most 0.5 deg/s per second
        omega += std::clamp(wcmd - omega, -rate, rate);
        st.trackDeg = wrap360(st.trackDeg + omega / kDeg);
        st.turnDegS = omega / kDeg;
        vWander += -vWander / 900.0 + 0.25 * r.n();
        st.speedMs += std::clamp(v - st.speedMs, -0.4, 0.8);
        st.vrateMs = vs;
        st.altM = std::max(0.0, st.altM + vs);
        stepPos(st, 1.0);
    }
    return tr;
}

// A holding pattern: 1 minute legs, standard-rate (3 deg/s) right turns, at 220 kt, 6000 m
inline Truth makeHold(double lat, double lon, double inboundDeg, double minutes) {
    Truth tr; tr.dt = 1;
    TState st{lat, lon, 6000, inboundDeg, 220 * kKt, 0, 0};
    int phase = 0, cnt = 0;
    for (int i = 0; i < (int)(minutes * 60); i++) {
        tr.s.push_back(st);
        if (phase == 0) { st.turnDegS = 0; if (++cnt >= 60) { phase = 1; cnt = 0; } }
        else if (phase == 1) { st.turnDegS = 3; st.trackDeg = wrap360(st.trackDeg + 3); if (++cnt >= 60) { phase = 2; cnt = 0; } }
        else if (phase == 2) { st.turnDegS = 0; if (++cnt >= 60) { phase = 3; cnt = 0; } }
        else { st.turnDegS = 3; st.trackDeg = wrap360(st.trackDeg + 3); if (++cnt >= 60) { phase = 0; cnt = 0; } }
        stepPos(st, 1.0);
    }
    return tr;
}

// ---- sensors ----------------------------------------------------------------------------------------------------------------
inline void offsetPos(double lat, double lon, double dE, double dN, double& la, double& lo) {
    const double d = std::hypot(dE, dN);
    if (d < 1e-6) { la = lat; lo = lon; return; }
    gcDestination(lat, lon, wrap360(std::atan2(dE, dN) / kDeg), d, la, lo);
}

struct AdsbSensor {
    double gapSec = 1.0, posSigmaM = 8.0, latencyMin = 0.1, latencyMax = 0.3, velSigma = 1.0, trkSigmaDeg = 0.8, dropout = 0.02, outlier = 0.003;
    int nacp = 9;
    Fix make(const Truth& tr, double t, Rng& r) const {
        const TState s = tr.at(t - (latencyMin + (latencyMax - latencyMin) * r.u()));
        Fix f; f.t = t; f.src = Src::Adsb; f.hasPos = true; f.nacp = nacp;
        offsetPos(s.lat, s.lon, posSigmaM * r.n(), posSigmaM * r.n(), f.lat, f.lon);
        if (r.u() < outlier) offsetPos(s.lat, s.lon, (r.u() * 2 - 1) * 8e4, (r.u() * 2 - 1) * 8e4, f.lat, f.lon);
        f.hasAlt = true; f.altM = std::round(s.altM / 7.62) * 7.62;
        f.hasVel = true; f.speedMs = s.speedMs + velSigma * r.n(); f.trackDeg = s.trackDeg + trkSigmaDeg * r.n();
        f.hasVrate = true; f.vrateMs = s.vrateMs + 0.5 * r.n();
        return f;
    }
};

// ACARS / VDL2 text position: written to a rounding step, taken some seconds before the message is received
struct SatSensor {
    double gapSec = 300, jitter = 0.2, resolutionDeg = 0.1 / 60, latencyMax = 30, outlier = 0.0;
    Src src = Src::Acars;
    bool withAlt = true;
    bool stamped = true;          // the report carries its own time (HHMMSS of the POS report, the ADS-C time stamp): the fix is dated with it
    double stampRes = 1.0;
    Fix make(const Truth& tr, double t, Rng& r) const {
        const double lat = latencyMax * r.u();
        const TState s = tr.at(t - lat);
        Fix f; f.t = t; f.src = src; f.hasPos = true; f.resolutionDeg = resolutionDeg;
        if (stamped) { f.t = std::round((t - lat) / stampRes) * stampRes; f.latencySec = 0; f.timeSigmaSec = 0.6; }
        f.lat = std::round(s.lat / resolutionDeg) * resolutionDeg;
        f.lon = std::round(s.lon / resolutionDeg) * resolutionDeg;
        if (f.lon > 180) f.lon -= 360;
        if (r.u() < outlier) { f.lat += 2.0 * (r.u() - 0.5); f.lon += 2.0 * (r.u() - 0.5); }
        if (withAlt) { f.hasAlt = true; f.altM = std::round(s.altM / 30.48) * 30.48; f.altSigmaM = 30; }
        return f;
    }
};

inline double errM(double la1, double lo1, double la2, double lo2) { return gcDistM(la1, lo1, la2, lo2); }

inline double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double x = p * (double)(v.size() - 1);
    const size_t i = (size_t)x;
    return i + 1 < v.size() ? v[i] + (x - (double)i) * (v[i + 1] - v[i]) : v[i];
}

}  // namespace tsim
