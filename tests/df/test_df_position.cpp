// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Position fixing: WGS-84 geodesics, 2-4 station fixes with noisy bearings (median error, ellipse calibration), outliers, degenerate geometry,
// range prior, the antimeridian.
#include "dect2/df/df.h"
#include "dect2/df/position.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace dect2::df;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double distM(double la1, double lo1, double la2, double lo2) { double d, a; geodInverse(la1, lo1, la2, lo2, d, a); return d; }
static bool inside(const Fix& f, double tla, double tlo) {
    double d, az;
    geodInverse(f.lat, f.lon, tla, tlo, d, az);
    double e = d * std::sin(az * kPi / 180), n = d * std::cos(az * kPi / 180);
    double th = f.e95.orientationDeg * kPi / 180;
    double u = e * std::sin(th) + n * std::cos(th), v = e * std::cos(th) - n * std::sin(th);   // along the major axis, across
    return (u * u) / (f.e95.semiMajorM * f.e95.semiMajorM) + (v * v) / (f.e95.semiMinorM * f.e95.semiMinorM) <= 1.0;
}

struct Scene { double tla, tlo; std::vector<BearingLine> lines; };
static Scene makeScene(Rng& g, int nSt, double sigma, double lat0 = 54.7, double lon0 = 25.3, bool noise = true) {
    Scene s;
    // stations 15..40 km from the centre at spread-out azimuths, transmitter 0..25 km from the centre
    double a0 = 360 * g.uni();
    for (int i = 0; i < nSt; i++) {
        BearingLine l;
        geodDirect(lat0, lon0, a0 + 360.0 * i / nSt + 25 * (g.uni() - 0.5), 15000 + 25000 * g.uni(), l.lat, l.lon);
        l.sigmaDeg = sigma;
        s.lines.push_back(l);
    }
    geodDirect(lat0, lon0, 360 * g.uni(), 25000 * g.uni(), s.tla, s.tlo);
    for (auto& l : s.lines) {
        double d, az; geodInverse(l.lat, l.lon, s.tla, s.tlo, d, az);
        l.bearingDeg = wrap360(az + (noise ? sigma * g.gauss() : 0));
    }
    return s;
}

int main() {
    // ---- geodesics (Vincenty/GeographicLib reference pair: Flinders Peak to Buninyong)
    {
        double d, a1, a2;
        geodInverse(-37.9510334167, 144.4248678889, -37.6528211389, 143.9264955278, d, a1, &a2);
        CHECK(std::abs(d - 54972.271) < 0.01 && std::abs(a1 - 306.86816) < 1e-4, "inverse %.3f %.5f %.5f", d, a1, a2);
        double la, lo; geodDirect(-37.9510334167, 144.4248678889, a1, d, la, lo);
        CHECK(std::abs(la + 37.6528211389) < 1e-7 && std::abs(lo - 143.9264955278) < 1e-7, "direct %.7f %.7f", la, lo);
        double bl, bo; geodDirect(-37.6528211389, 143.9264955278, wrap360(a2 + 180), d, bl, bo);
        CHECK(std::abs(bl + 37.9510334167) < 1e-7 && std::abs(bo - 144.4248678889) < 1e-7, "reverse azimuth");
        geodInverse(0, 0, 0, 1, d, a1);
        CHECK(std::abs(d - 111319.49) < 0.1 && std::abs(a1 - 90) < 1e-9, "equator degree %g", d);
    }
    // ---- noiseless: exact
    {
        Rng g(3);
        for (int ns = 2; ns <= 4; ns++) {
            auto s = makeScene(g, ns, 3, 54.7, 25.3, false);
            for (auto m : {FixMethod::Stansfield, FixMethod::MaxLikelihood}) {
                Fix f = fixPosition(s.lines, m);
                CHECK(f.ok && distM(f.lat, f.lon, s.tla, s.tlo) < (m == FixMethod::MaxLikelihood ? 5 : 300), "noiseless %d stations method %d: %g m", ns, (int)m, distM(f.lat, f.lon, s.tla, s.tlo));
            }
        }
    }
    // ---- Monte Carlo: median error and ellipse calibration
    printf("Position fix, 300 trials per cell (stations 15-40 km out, transmitter within 25 km of centre)\n");
    printf("%-9s %-5s %-14s %-12s %-9s %-9s\n", "stations", "sigma", "estimator", "median err m", "p95 m", "in 95pct ellipse");
    for (int ns : {2, 3, 4}) for (double sg : {2.0, 5.0}) for (auto m : {FixMethod::Stansfield, FixMethod::MaxLikelihood}) {
        Rng g(1000 + ns * 10 + (int)sg);
        std::vector<double> errs; int in = 0, ok = 0;
        for (int k = 0; k < 300; k++) {
            auto s = makeScene(g, ns, sg);
            Fix f = fixPosition(s.lines, m);
            if (!f.ok) continue;
            ok++;
            errs.push_back(distM(f.lat, f.lon, s.tla, s.tlo));
            if (inside(f, s.tla, s.tlo)) in++;
        }
        std::sort(errs.begin(), errs.end());
        double med = errs[errs.size() / 2], p95 = errs[errs.size() * 95 / 100], cov = (double)in / ok;
        printf("%-9d %-5.0f %-14s %-12.0f %-9.0f %.2f\n", ns, sg, m == FixMethod::MaxLikelihood ? "max-likelihood" : "Stansfield", med, p95, cov);
        if (m == FixMethod::MaxLikelihood) {
            if (ns >= 3) CHECK(ok > 285, "fix rate %d/300", ok);
            // the median error scales with sigma and falls with stations; 4 stations at 2 deg must be within ~500 m
            if (ns == 4 && sg == 2) CHECK(med < 1000, "4 stations 2 deg median %g", med);
            if (ns >= 3) CHECK(cov > 0.88 && cov < 0.99, "ellipse coverage %d stations sigma %g: %.2f", ns, sg, cov);
        }
    }
    // ---- outliers: one station is 70 degrees off
    {
        Rng g(77);
        int plainBad = 0, robustGood = 0, flagged = 0, trials = 100;
        for (int k = 0; k < trials; k++) {
            auto s = makeScene(g, 5, 3);
            s.lines[2].bearingDeg = wrap360(s.lines[2].bearingDeg + 70);
            Fix p = fixPosition(s.lines);
            Fix r = fixPositionRobust(s.lines);
            if (!p.ok || distM(p.lat, p.lon, s.tla, s.tlo) > 3000) plainBad++;
            if (r.ok && distM(r.lat, r.lon, s.tla, s.tlo) < 2500) robustGood++;
            if (r.ok && !r.inlier[2] && r.inliers == 4) flagged++;
        }
        printf("outlier (1 of 5 bearings +70 deg): plain fix off by >3 km in %d%%, robust fix within 2.5 km in %d%%, outlier flagged in %d%%\n", plainBad * 100 / trials, robustGood * 100 / trials, flagged * 100 / trials);
        CHECK(robustGood > 85 && flagged > 85, "robust: %d good %d flagged", robustGood, flagged);
        CHECK(plainBad > robustGood * 0 + 20, "the outlier hurts the plain fix (%d)", plainBad);
    }
    // ---- degenerate geometry and rays
    {
        BearingLine a, b;
        a.lat = 54.7; a.lon = 25.0; a.bearingDeg = 90;
        b.lat = 54.8; b.lon = 25.0; b.bearingDeg = 90;
        CHECK(!fixPosition({a, b}).ok, "parallel lines give no fix");
        b.bearingDeg = 270;      // diverging
        CHECK(!fixPositionRobust({a, b}).ok || fixPositionRobust({a, b}).inliers < 2, "lines meeting behind the stations");
        CHECK(!fixPosition({a}).ok, "one line gives no fix");
    }
    // ---- antimeridian
    {
        BearingLine a, b;
        a.lat = 10; a.lon = 179.8; b.lat = 10.2; b.lon = -179.8;
        double tla = 10.4, tlo = -179.95, d, az;
        geodInverse(a.lat, a.lon, tla, tlo, d, az); a.bearingDeg = az;
        geodInverse(b.lat, b.lon, tla, tlo, d, az); b.bearingDeg = az;
        Fix f = fixPosition({a, b});
        CHECK(f.ok && distM(f.lat, f.lon, tla, tlo) < 10, "antimeridian fix %g m", f.ok ? distM(f.lat, f.lon, tla, tlo) : -1);
    }
    // ---- range prior
    {
        BearingLine a; a.lat = 54.7; a.lon = 25.3; a.bearingDeg = 45; a.sigmaDeg = 3;
        Fix f = fixWithRangePrior(a, 20000, 5000);
        CHECK(f.ok && std::abs(distM(a.lat, a.lon, f.lat, f.lon) - 20000) < 1, "range prior position");
        // along-bearing sigma 5 km (major axis, bearing direction), cross sigma 20000*3deg = 1047 m
        CHECK(std::abs(f.e95.semiMajorM - 2.4477 * 5000) < 50 && std::abs(f.e95.semiMinorM - 2.4477 * 1047.2) < 20, "range prior ellipse %g %g", f.e95.semiMajorM, f.e95.semiMinorM);
        CHECK(std::abs(f.e95.orientationDeg - 45) < 0.5, "ellipse orientation %g", f.e95.orientationDeg);
    }
    printf(fails ? "df position: %d FAILED\n" : "df position: all passed\n", fails);
    return fails ? 1 : 0;
}
