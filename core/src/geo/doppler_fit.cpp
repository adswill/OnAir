// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Doppler-curve orbit refinement: see doppler_fit.h
#include "dect2/geo/doppler_fit.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {

bool DopplerTracker::feed(double t, const float* db, size_t n, double centerHz, double fs, double nominalHz, double predictedHz, double gateHz, DopplerSample& out) const {
    if (n < 8 || fs <= 0 || gateHz <= 0) return false;
    const double bin = fs / double(n);
    // bin k holds the offset (k - n/2) * bin from the centre
    const double lo = (predictedHz - gateHz - centerHz) / bin + n / 2.0, hi = (predictedHz + gateHz - centerHz) / bin + n / 2.0;
    const long a = std::max(1L, (long)std::ceil(lo)), b = std::min((long)n - 2, (long)std::floor(hi));
    if (b - a < 4) return false;
    long best = a;
    std::vector<float> w;
    w.reserve(size_t(b - a + 1));
    for (long k = a; k <= b; k++) { w.push_back(db[k]); if (db[k] > db[best]) best = k; }
    std::nth_element(w.begin(), w.begin() + w.size() / 2, w.end());
    if (db[best] - w[w.size() / 2] < minSnrDb) return false;
    // parabolic interpolation of the peak in dB
    const double y0 = db[best - 1], y1 = db[best], y2 = db[best + 1], den = y0 - 2 * y1 + y2;
    const double d = den < -1e-9 ? 0.5 * (y0 - y2) / den : 0.0;
    out.t = t;
    out.hz = centerHz + (best + std::max(-0.5, std::min(0.5, d)) - n / 2.0) * bin - nominalHz;
    out.sigmaHz = 0;
    return true;
}

namespace {
constexpr int NP = 5;   // dn, dM, di, dRaan, offset

Tle perturbed(const Tle& t, const double* p) {
    Tle x = t;
    x.meanMotion += p[0]; x.meanAnomDeg += p[1]; x.inclDeg += p[2]; x.raanDeg += p[3];
    return x;
}

// model Doppler (Hz, without the receiver offset); false when the propagator fails
bool model(const Sgp4& s, const Observer& o, double nominalHz, const std::vector<DopplerSample>& smp, std::vector<double>& m) {
    m.resize(smp.size());
    for (size_t i = 0; i < smp.size(); i++) {
        Look l;
        if (!lookAt(s, o, smp[i].t, l)) return false;
        m[i] = dopplerHz(nominalHz, l.rangeRateKmS);
    }
    return true;
}

// solve the NxN system A x = b by Gaussian elimination with pivoting
bool solve(double A[NP][NP], double* b, int n, double* x) {
    double M[NP][NP + 1];
    for (int i = 0; i < n; i++) { for (int j = 0; j < n; j++) M[i][j] = A[i][j]; M[i][n] = b[i]; }
    for (int c = 0; c < n; c++) {
        int p = c;
        for (int r = c + 1; r < n; r++) if (std::fabs(M[r][c]) > std::fabs(M[p][c])) p = r;
        if (std::fabs(M[p][c]) < 1e-300) return false;
        if (p != c) for (int j = 0; j <= n; j++) std::swap(M[p][j], M[c][j]);
        for (int r = 0; r < n; r++) {
            if (r == c) continue;
            const double f = M[r][c] / M[c][c];
            for (int j = c; j <= n; j++) M[r][j] -= f * M[c][j];
        }
    }
    for (int i = 0; i < n; i++) x[i] = M[i][n] / M[i][i];
    return true;
}

bool invert(double A[NP][NP], int n, double Inv[NP][NP]) {
    for (int c = 0; c < n; c++) {
        double e[NP] = {0}, col[NP];
        e[c] = 1;
        if (!solve(A, e, n, col)) return false;
        for (int r = 0; r < n; r++) Inv[r][c] = col[r];
    }
    return true;
}
}

bool fitDopplerOrbit(const Tle& tle, const Observer& obs, double nominalHz, const std::vector<DopplerSample>& samples, const DopplerFitOptions& opt, DopplerFit& out) {
    out = DopplerFit();
    const int N = (int)samples.size();
    if (N < 12) { out.why = "too few measurements"; return false; }
    if (samples.back().t - samples.front().t < 60 && samples.back().t - samples.front().t > -60) { out.why = "the measurements cover less than a minute"; return false; }
    Sgp4 base;
    if (!base.init(tle)) { out.why = "the elements cannot be propagated"; return false; }
    const int np = opt.fitOffset ? NP : NP - 1;
    const double prior[NP] = {opt.priorMeanMotion, opt.priorMeanAnomDeg, opt.priorInclDeg, opt.priorRaanDeg, opt.offsetPriorHz};
    const double step[NP] = {1e-5, 1e-3, 1e-3, 1e-3, 1.0};
    std::vector<double> sig(N), m0;
    for (int i = 0; i < N; i++) sig[i] = samples[i].sigmaHz > 0 ? samples[i].sigmaHz : opt.defaultSigmaHz;
    if (!model(base, obs, nominalHz, samples, m0)) { out.why = "the propagator failed"; return false; }
    out.initialHz.resize(N);
    for (int i = 0; i < N; i++) out.initialHz[i] = samples[i].hz - m0[i];

    std::vector<char> use(N, 1);
    double p[NP] = {0, 0, 0, 0, 0};
    double cov[NP][NP] = {};
    std::vector<double> m, mj;
    for (int pass = 0; pass < 3; pass++) {
        double lambda = 1e-2;
        auto cost = [&](const double* q, std::vector<double>& mm) -> double {
            Sgp4 s;
            if (!s.init(perturbed(tle, q)) || !model(s, obs, nominalHz, samples, mm)) return 1e300;
            double c = 0;
            for (int i = 0; i < N; i++) if (use[i]) { const double r = (samples[i].hz - mm[i] - q[4]) / sig[i]; c += r * r; }
            for (int k = 0; k < np; k++) c += (q[k] / prior[k]) * (q[k] / prior[k]);
            return c;
        };
        double c = cost(p, m);
        if (c >= 1e299) { out.why = "the propagator failed"; return false; }
        for (int it = 0; it < opt.maxIter; it++) {
            // Jacobian by forward differences
            std::vector<std::vector<double>> col(np, std::vector<double>(N));
            for (int k = 0; k < np; k++) {
                double q[NP]; std::copy(p, p + NP, q);
                q[k] += step[k];
                if (k == 4) { for (int i = 0; i < N; i++) col[k][i] = 1.0; continue; }   // d(model + offset)/d offset
                if (cost(q, mj) >= 1e299) { out.why = "the propagator failed"; return false; }
                for (int i = 0; i < N; i++) col[k][i] = (mj[i] - m[i]) / step[k];   // d model / d p
            }
            // residual r_i = meas - model - off; d r/dp = -d model/dp, d r/d off = -1
            double H[NP][NP] = {}, g[NP] = {};
            for (int k = 0; k < np; k++) {
                for (int l = 0; l < np; l++) {
                    double s = 0;
                    for (int i = 0; i < N; i++) if (use[i]) s += col[k][i] * col[l][i] / (sig[i] * sig[i]);
                    H[k][l] = s;
                }
                H[k][k] += 1.0 / (prior[k] * prior[k]);
                double s = 0;
                for (int i = 0; i < N; i++) if (use[i]) s += col[k][i] * (samples[i].hz - m[i] - p[4]) / (sig[i] * sig[i]);
                g[k] = s - p[k] / (prior[k] * prior[k]);
            }
            for (int k = 0; k < np; k++) for (int l = 0; l < np; l++) cov[k][l] = H[k][l];
            bool improved = false;
            for (int tries = 0; tries < 12 && !improved; tries++) {
                double A[NP][NP], dp[NP];
                for (int k = 0; k < np; k++) { for (int l = 0; l < np; l++) A[k][l] = H[k][l]; A[k][k] *= 1 + lambda; }
                if (!solve(A, g, np, dp)) { lambda *= 10; continue; }
                double q[NP] = {0, 0, 0, 0, p[4]};
                for (int k = 0; k < np; k++) q[k] = p[k] + dp[k];
                std::vector<double> mq;
                const double cq = cost(q, mq);
                if (cq < c) {
                    const double gain = c - cq;
                    std::copy(q, q + NP, p); m = mq; c = cq; lambda = std::max(1e-7, lambda / 5); improved = true;
                    if (gain < 1e-6 * (1 + c)) it = opt.maxIter;
                } else lambda *= 8;
            }
            if (!improved) break;
        }
        // outlier rejection: against the larger of the assumed sigma and the robust scatter of the residuals (median absolute deviation)
        if (pass < 2 && opt.rejectSigma > 0) {
            std::vector<double> ad;
            for (int i = 0; i < N; i++) if (use[i]) ad.push_back(std::fabs(samples[i].hz - m[i] - p[4]));
            std::nth_element(ad.begin(), ad.begin() + ad.size() / 2, ad.end());
            const double scale = std::max(1.0, 1.4826 * ad[ad.size() / 2]);
            int dropped = 0, left = 0;
            std::vector<char> nu = use;
            for (int i = 0; i < N; i++) {
                if (!use[i]) continue;
                if (std::fabs(samples[i].hz - m[i] - p[4]) > opt.rejectSigma * std::max(sig[i], scale)) { nu[i] = 0; dropped++; } else left++;
            }
            if (!dropped || left < 12) break;
            use = nu;
        } else break;
    }
    // covariance of the final fit (inverse of the information matrix at the solution)
    double H[NP][NP] = {}, Inv[NP][NP] = {};
    {
        std::vector<std::vector<double>> col(np, std::vector<double>(N));
        for (int k = 0; k < np; k++) {
            if (k == 4) { for (int i = 0; i < N; i++) col[k][i] = 1.0; continue; }
            double q[NP]; std::copy(p, p + NP, q); q[k] += step[k];
            Sgp4 s; std::vector<double> mq;
            if (!s.init(perturbed(tle, q)) || !model(s, obs, nominalHz, samples, mq)) { out.why = "the propagator failed"; return false; }
            for (int i = 0; i < N; i++) col[k][i] = (mq[i] - m[i]) / step[k];
        }
        for (int k = 0; k < np; k++) {
            for (int l = 0; l < np; l++) { double s = 0; for (int i = 0; i < N; i++) if (use[i]) s += col[k][i] * col[l][i] / (sig[i] * sig[i]); H[k][l] = s; }
            H[k][k] += 1.0 / (prior[k] * prior[k]);
        }
        if (!invert(H, np, Inv)) { out.why = "the fit is degenerate"; return false; }
    }
    out.used = 0;
    double ss0 = 0, ss1 = 0;
    out.predictedHz.resize(N); out.residualHz.resize(N);
    for (int i = 0; i < N; i++) {
        out.predictedHz[i] = m[i] + p[4];
        out.residualHz[i] = samples[i].hz - out.predictedHz[i];
        if (use[i]) { out.used++; ss1 += out.residualHz[i] * out.residualHz[i]; const double r0 = out.initialHz[i]; ss0 += r0 * r0; }
    }
    out.rejected = N - out.used;
    out.rmsBeforeHz = std::sqrt(ss0 / out.used);
    out.rmsAfterHz = std::sqrt(ss1 / out.used);
    out.dMeanMotion = p[0]; out.dMeanAnomDeg = p[1]; out.dInclDeg = p[2]; out.dRaanDeg = p[3]; out.offsetHz = opt.fitOffset ? p[4] : 0;
    // timing error: mean anomaly error divided by the mean anomaly rate (deg/s); inflate by the residual scatter relative to the assumed sigma
    const double degPerSec = tle.meanMotion * 360.0 / 86400.0;
    const double chi = std::max(1.0, out.rmsAfterHz / opt.defaultSigmaHz);
    const double tMid = 0.5 * (samples.front().t + samples.back().t), dtDays = (tMid - jdToUnix(tle.epochJd)) / 86400.0;
    const double gdn = 360.0 * dtDays;   // the mean anomaly shift at the pass from a mean-motion change, degrees per (rev/day)
    const double var = Inv[1][1] + 2 * gdn * Inv[0][1] + gdn * gdn * Inv[0][0];
    out.alongTrackSigmaSec = std::sqrt(std::max(0.0, var)) * chi / degPerSec;
    out.alongTrackSigmaBeforeSec = std::sqrt(opt.priorMeanAnomDeg * opt.priorMeanAnomDeg + gdn * gdn * opt.priorMeanMotion * opt.priorMeanMotion) / degPerSec;
    out.refined = perturbed(tle, p);
    out.refined.name = (tle.name.empty() ? std::to_string(tle.norad) : tle.name) + " (derived)";
    out.ok = true;
    return true;
}

void formatTle(const Tle& t, std::string& l1, std::string& l2) {
    // epoch: year and day of year from the UTC Julian date
    const double unixSec = jdToUnix(t.epochJd);
    long days = (long)std::floor(unixSec / 86400.0);
    const double frac = unixSec / 86400.0 - days;
    // civil year from days since 1970-01-01
    long z = days + 719468; const long era = (z >= 0 ? z : z - 146096) / 146097; const long doe = z - era * 146097;
    const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; long y = yoe + era * 400;
    const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100); const long mp = (5 * doy + 2) / 153;
    const long m = mp < 10 ? mp + 3 : mp - 9; if (m <= 2) y++;
    auto leap = [](long yy) { return (yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0; };
    long dayOfYear = 0;
    { static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
      const long dd = doy - ((153 * mp + 2) / 5) + 1;   // day of month
      dayOfYear = cum[m - 1] + dd + (m > 2 && leap(y) ? 1 : 0); }
    // B* as the TLE's mantissa/exponent: sign, five digits (assumed leading point), exponent
    char bs[16];
    {
        double v = t.bstar; const char sgn = v < 0 ? '-' : ' '; v = std::fabs(v);
        int ex = 0;
        if (v < 1e-12) snprintf(bs, sizeof bs, " 00000-0");
        else {
            while (v >= 1.0) { v /= 10; ex++; }
            while (v < 0.1) { v *= 10; ex--; }
            long mant = std::lround(v * 1e5);
            if (mant >= 100000) { mant = 10000; ex++; }
            snprintf(bs, sizeof bs, "%c%05ld%c%d", sgn, mant, ex < 0 ? '-' : '+', std::abs(ex));
        }
    }
    {
        char c[96];
        snprintf(c, sizeof c, "1 %05dU 00000A   %02ld%012.8f  .00000000  00000-0 %s 0  999", t.norad, y % 100, dayOfYear + frac, bs);
        l1 = c;
        l1.resize(68, ' ');
    }
    char c2[96];
    snprintf(c2, sizeof c2, "2 %05d %8.4f %8.4f %07ld %8.4f %8.4f %11.8f%5d", t.norad, t.inclDeg, std::fmod(std::fmod(t.raanDeg, 360) + 360, 360),
             std::lround(t.ecc * 1e7), std::fmod(std::fmod(t.argpDeg, 360) + 360, 360), std::fmod(std::fmod(t.meanAnomDeg, 360) + 360, 360), t.meanMotion, 0);
    l2 = c2;
    l2.resize(68, ' ');
    auto sum = [](const std::string& s) { int a = 0; for (char ch : s) { if (ch >= '0' && ch <= '9') a += ch - '0'; else if (ch == '-') a++; } return a % 10; };
    l1 += char('0' + sum(l1));
    l2 += char('0' + sum(l2));
}

std::string derivedTleText(const DopplerFit& f, double nominalHz) {
    std::string l1, l2;
    formatTle(f.refined, l1, l2);
    char h[200];
    snprintf(h, sizeof h, "# DERIVED from a measured Doppler curve at %.4f MHz (rms %.1f -> %.1f Hz): not an official element set\n", nominalHz / 1e6, f.rmsBeforeHz, f.rmsAfterHz);
    return std::string(h) + f.refined.name + "\n" + l1 + "\n" + l2 + "\n";
}

}
