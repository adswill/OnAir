// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sun drift-scan pointing calibration (see drift_scan.h).
#include "dect2/smart5/drift_scan.h"
#include "dect2/smart5/sun.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace smart5 {

namespace {
const double kPi = 3.14159265358979323846, kD = kPi / 180;
const double kSunDiskFwhm = 0.589 * 0.533;   // a uniform disk of 0.533 deg as a Gaussian of the same second moment

double sepToDish(double t, double lat, double lon, double az, double el) {
    double sa, se; sunAzEl(t, lat, lon, sa, se);
    const double c = std::sin(se * kD) * std::sin(el * kD) + std::cos(se * kD) * std::cos(el * kD) * std::cos((sa - az) * kD);
    return std::acos(std::max(-1.0, std::min(1.0, c))) / kD;
}

// weighted least squares y = a + b x + c x^2
bool quadFit(const std::vector<double>& x, const std::vector<double>& y, const std::vector<double>& w, double& a, double& b, double& c) {
    double S[3][4] = {{0}};
    for (size_t i = 0; i < x.size(); i++) {
        const double v[3] = {1, x[i], x[i] * x[i]};
        for (int r = 0; r < 3; r++) { for (int k = 0; k < 3; k++) S[r][k] += w[i] * v[r] * v[k]; S[r][3] += w[i] * v[r] * y[i]; }
    }
    for (int i = 0; i < 3; i++) {
        int pv = i; for (int k = i + 1; k < 3; k++) if (std::fabs(S[k][i]) > std::fabs(S[pv][i])) pv = k;
        for (int k = 0; k < 4; k++) std::swap(S[i][k], S[pv][k]);
        if (std::fabs(S[i][i]) < 1e-300) return false;
        for (int k = i + 1; k < 3; k++) { const double m = S[k][i] / S[i][i]; for (int q = i; q < 4; q++) S[k][q] -= m * S[i][q]; }
    }
    double r[3];
    for (int i = 2; i >= 0; i--) { double s = S[i][3]; for (int k = i + 1; k < 3; k++) s -= S[i][k] * r[k]; r[i] = s / S[i][i]; }
    a = r[0]; b = r[1]; c = r[2];
    return true;
}
}

DriftScanResult driftScan(const std::vector<double>& t, const std::vector<double>& p, double lat, double lon, double daz, double del, double expY) {
    DriftScanResult r;
    const size_t n = std::min(t.size(), p.size());
    if (n < 50) return r;
    // baseline: a line through the medians of the first and last 15 %
    const size_t e = std::max<size_t>(5, n * 15 / 100);
    auto med = [&](size_t a, size_t b, const std::vector<double>& v) { std::vector<double> s(v.begin() + a, v.begin() + b); std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end()); return s[s.size() / 2]; };
    const double b0 = med(0, e, p), b1 = med(n - e, n, p), t0 = med(0, e, t), t1 = med(n - e, n, t);
    auto base = [&](double x) { return b0 + (b1 - b0) * (x - t0) / (t1 - t0); };
    // the peak of a 9-point running mean
    size_t ip = 0; double best = -1e300;
    for (size_t i = 4; i + 4 < n; i++) {
        double s = 0; for (size_t k = i - 4; k <= i + 4; k++) s += p[k] - base(t[k]);
        if (s > best) { best = s; ip = i; }
    }
    const double A0 = best / 9;
    if (A0 <= 0) return r;
    // Gaussian from the log of the samples above 30 % of the peak, weighted by their square (noise is additive)
    std::vector<double> xs, ys, ws;
    const double tc = t[ip];
    for (size_t i = 0; i < n; i++) {
        const double y = p[i] - base(t[i]);
        if (y > 0.3 * A0) { xs.push_back(t[i] - tc); ys.push_back(std::log(y)); ws.push_back(y * y); }
    }
    double a, b, c;
    if (xs.size() < 10 || !quadFit(xs, ys, ws, a, b, c) || c >= 0) return r;
    const double tpk = tc - b / (2 * c);
    const double A = std::exp(a - b * b / (4 * c));
    r.valid = true;
    r.peakTime = tpk;
    r.fwhmSec = std::sqrt(-4 * std::log(2.0) / c);
    const double dec = sunAt(tpk).decDeg;
    const double rate = 15.0410686 / 3600 * std::cos(dec * kD);   // deg per second along the Sun's path
    const double obs = r.fwhmSec * rate;
    r.beamDeg = obs > kSunDiskFwhm ? std::sqrt(obs * obs - kSunDiskFwhm * kSunDiskFwhm) : obs;
    const double B = base(tpk);
    r.yFactorDb = 10 * std::log10((B + A) / B);
    // when the Sun was closest to the stated pointing
    double tPred = tpk, sBest = 1e9;
    for (double x = t[0]; x <= t[n - 1]; x += 1) { const double s = sepToDish(x, lat, lon, daz, del); if (s < sBest) { sBest = s; tPred = x; } }
    for (double step = 0.5; step > 0.01; step *= 0.5)
        for (double x : {tPred - step, tPred + step}) { const double s = sepToDish(x, lat, lon, daz, del); if (s < sBest) { sBest = s; tPred = x; } }
    r.alongErrDeg = (tpk - tPred) * rate;
    double az0, el0, az1, el1;
    sunAzEl(tPred, lat, lon, az0, el0);
    sunAzEl(tpk, lat, lon, az1, el1);
    double daz2 = az1 - az0; if (daz2 > 180) daz2 -= 360; if (daz2 < -180) daz2 += 360;
    r.azCorrDeg = daz2; r.elCorrDeg = el1 - el0;
    if (expY > 0) {
        // the shortfall of the peak against the expected Sun excess gives the offset across the path
        const double Aexp = B * (std::pow(10.0, expY / 10) - 1);
        const double th = std::sqrt(r.beamDeg * r.beamDeg + kSunDiskFwhm * kSunDiskFwhm);
        r.crossErrDeg = A < Aexp ? th * std::sqrt(std::log(Aexp / A) / (4 * std::log(2.0))) : 0;
    }
    return r;
}

std::string driftScanText(const DriftScanResult& r) {
    if (!r.valid) return "drift scan: no Sun transit found";
    char b[220];
    snprintf(b, sizeof b, "beam %.2f deg, Sun/sky %.1f dB, pointing off %.2f deg along the Sun's path (az %+.2f, el %+.2f)%s",
             r.beamDeg, r.yFactorDb, r.alongErrDeg, r.azCorrDeg, r.elCorrDeg, r.crossErrDeg >= 0 ? "" : "; scan again at another hour for the cross error");
    return b;
}

} // namespace smart5
} // namespace dect2
