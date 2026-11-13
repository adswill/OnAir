// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// IMM position estimator, see track_imm.h and docs/position-estimation.md.
#include "dect2/track/track_imm.h"
#include <algorithm>
#include <cmath>

namespace dect2::track {

const char* srcName(Src s) {
    switch (s) { case Src::Adsb: return "ADS-B"; case Src::Uat: return "UAT"; case Src::Acars: return "ACARS"; case Src::Vdl2: return "VDL2"; case Src::Hfdl: return "HFDL"; default: return "other"; }
}

// ---- measurement noise -------------------------------------------------------------------------------------------------------
double nacpEpuM(int n) {   // EPU (95 %) bound of NACp, DO-260B table 2-xx / ICAO Doc 9871: 11: 3 m, 10: 10 m, 9: 30 m, 8: 0.05 nm, 7: 0.1, 6: 0.3, 5: 0.5, 4: 1, 3: 2, 2: 4, 1: 10 nm
    static const double t[12] = {0, 18520, 7408, 3704, 1852, 926, 555.6, 185.2, 92.6, 30, 10, 3};
    return n >= 1 && n <= 11 ? t[n] : 0;
}
double nicRcM(int n) {     // containment radius Rc of NIC (barometric-supplement variants not distinguished)
    static const double t[12] = {0, 37040, 14816, 7408, 3704, 1852, 1111.2, 370.4, 185.2, 75, 25, 7.5};
    return n >= 1 && n <= 11 ? t[n] : 0;
}

static double defaultTimeSigma(Src s) {
    switch (s) { case Src::Adsb: case Src::Uat: return 0.1; case Src::Vdl2: return 8; case Src::Acars: return 20; case Src::Hfdl: return 20; default: return 5; }
}

double fixPosSigmaM(const Fix& f, double speedMs) {
    if (f.posSigmaM > 0) return f.posSigmaM;
    double s = 0;
    const bool air = f.src == Src::Adsb || f.src == Src::Uat;
    if (air) {
        const double a = nacpEpuM(f.nacp) / 2.448, b = nicRcM(f.nic) / 5.0;      // 2-D Rayleigh: r95 = 2.448 sigma; Rc is a 1e-7 containment radius
        s = a > 0 ? a : b > 0 ? b : 25.0;
        s = std::max(s, 6.0);                                                    // CPR quantisation and latency
    } else {
        s = f.src == Src::Hfdl ? 150.0 : 60.0;                                   // ADS-C encodes lat/lon in 21 bits (about 20 m); positions are filtered by the FMS
    }
    if (f.resolutionDeg > 0) { const double q = f.resolutionDeg * 111320.0 / std::sqrt(12.0); s = std::sqrt(s * s + q * q); }
    const double ts = f.timeSigmaSec >= 0 ? f.timeSigmaSec : defaultTimeSigma(f.src);
    const double v = speedMs > 0 ? speedMs : 230.0;
    return std::sqrt(s * s + v * v * ts * ts);
}

// ---- small linear algebra ----------------------------------------------------------------------------------------------------
template <int N> static bool invert(const Mat<N, N>& a, Mat<N, N>& out, double* logDet = nullptr) {
    Mat<N, N> m = a, inv = Mat<N, N>::eye();
    double ld = 0;
    for (int c = 0; c < N; c++) {
        int p = c;
        for (int r = c + 1; r < N; r++) if (std::fabs(m(r, c)) > std::fabs(m(p, c))) p = r;
        if (!(std::fabs(m(p, c)) > 1e-300)) return false;
        if (p != c) for (int j = 0; j < N; j++) { std::swap(m(p, j), m(c, j)); std::swap(inv(p, j), inv(c, j)); }
        const double d = m(c, c);
        ld += std::log(std::fabs(d));
        for (int j = 0; j < N; j++) { m(c, j) /= d; inv(c, j) /= d; }
        for (int r = 0; r < N; r++) if (r != c) { const double f = m(r, c); if (f == 0) continue; for (int j = 0; j < N; j++) { m(r, j) -= f * m(c, j); inv(r, j) -= f * inv(c, j); } }
    }
    out = inv;
    if (logDet) *logDet = ld;
    return true;
}

static double clampd(double v, double a, double b) { return v < a ? a : v > b ? b : v; }

// ---- horizontal models -------------------------------------------------------------------------------------------------------
using V5 = Mat<5, 1>;
using M5 = Mat<5, 5>;
enum { kCV = 0, kCT = 1, kMAN = 2 };

static void propagate(int j, V5& x, M5& P, double T, const TrackOptions& o, bool cov, double qs) {
    M5 F = M5::eye();
    const double e = x(0, 0), n = x(1, 0), ve = x(2, 0), vn = x(3, 0), w = x(4, 0);
    double qa = (j == kCV ? o.qCv : j == kCT ? o.qCt : o.qMan) * o.covInflate * qs;
    double qw = j == kCT ? o.qOmega : 1e-12;
    V5 y;
    if (j != kCT) {
        y(0, 0) = e + ve * T; y(1, 0) = n + vn * T; y(2, 0) = ve; y(3, 0) = vn; y(4, 0) = 0;
        F(0, 2) = T; F(1, 3) = T; F(4, 4) = 0;
    } else {
        const double th = w * T, st = std::sin(th), ct = std::cos(th), a = std::exp(-T / o.omegaTauSec);
        double S, C, dS, dC;
        if (std::fabs(th) < 1e-3) { S = T * (1 - th * th / 6); C = T * th / 2 * (1 - th * th / 12); dS = -T * T * th / 3; dC = T * T / 2 * (1 - th * th / 4); }
        else { S = st / w; C = (1 - ct) / w; dS = (T * ct - S) / w; dC = (T * st - C) / w; }
        y(0, 0) = e + S * ve - C * vn; y(1, 0) = n + C * ve + S * vn;
        y(2, 0) = ct * ve - st * vn; y(3, 0) = st * ve + ct * vn; y(4, 0) = a * w;
        F(0, 2) = S; F(0, 3) = -C; F(0, 4) = dS * ve - dC * vn;
        F(1, 2) = C; F(1, 3) = S; F(1, 4) = dC * ve + dS * vn;
        F(2, 2) = ct; F(2, 3) = -st; F(2, 4) = T * (-st * ve - ct * vn);
        F(3, 2) = st; F(3, 3) = ct; F(3, 4) = T * (ct * ve - st * vn);
        F(4, 4) = a;
    }
    x = y;
    if (!cov) return;
    M5 Q;
    const double q3 = qa * T * T * T / 3, q2 = qa * T * T / 2, q1 = qa * T;
    Q(0, 0) = Q(1, 1) = q3; Q(0, 2) = Q(2, 0) = Q(1, 3) = Q(3, 1) = q2; Q(2, 2) = Q(3, 3) = q1;
    Q(4, 4) = qw * T;
    P = F * P * tr(F) + Q;
    symmetrize(P);
}

static void transition(const TrackOptions& o, double dt, double Pi[3][3]) {
    static const double w[3][3] = {{0, 0.5, 0.5}, {0.6, 0, 0.4}, {0.6, 0.4, 0}};
    for (int i = 0; i < 3; i++) {
        const double leave = 1 - std::exp(-dt / o.sojournSec[i]);
        for (int j = 0; j < 3; j++) Pi[i][j] = i == j ? 1 - leave : leave * w[i][j];
    }
}

// mixing, then prediction of every model over T
static void immPredict(TrackEstimator::Core& c, double T, const TrackOptions& o, bool cov) {
    double Pi[3][3];
    transition(o, T, Pi);
    double cb[3] = {0, 0, 0};
    for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++) cb[j] += Pi[i][j] * c.hmu[i];
    V5 x0[3]; M5 P0[3];
    for (int j = 0; j < 3; j++) {
        const double cj = std::max(cb[j], 1e-300);
        for (int i = 0; i < 3; i++) { const double m = Pi[i][j] * c.hmu[i] / cj; for (int k = 0; k < 5; k++) x0[j](k, 0) += m * c.hx[i](k, 0); }
        if (cov) for (int i = 0; i < 3; i++) {
            const double m = Pi[i][j] * c.hmu[i] / cj;
            V5 d; for (int k = 0; k < 5; k++) d(k, 0) = c.hx[i](k, 0) - x0[j](k, 0);
            P0[j] = P0[j] + m * (c.hP[i] + d * tr(d));
        }
    }
    const double vh = c.vmu[0] * c.vx[0](1, 0) + c.vmu[1] * c.vx[1](1, 0), vr = std::min(1.0, (vh / 6.0) * (vh / 6.0));
    const double vertBoost = 1.0 + o.climbQBoost * vr;
    for (int j = 0; j < 3; j++) { c.hx[j] = x0[j]; c.hP[j] = P0[j]; c.hmu[j] = cb[j]; propagate(j, c.hx[j], c.hP[j], T, o, cov, c.qScale * vertBoost); }
    double s = 0; for (int j = 0; j < 3; j++) s += c.hmu[j];
    for (int j = 0; j < 3; j++) c.hmu[j] /= s;
}

static void predictTo(TrackEstimator::Core& c, double tNew, const TrackOptions& o, bool cov) {
    const double dt = tNew - c.t;
    if (dt <= 0) return;
    const int steps = std::max(1, (int)std::ceil(dt / o.maxStepSec));
    const double T = dt / steps;
    for (int s = 0; s < steps; s++) immPredict(c, T, o, cov);
    c.t = tNew;
}

// Kalman update of one model with an m-dim linear measurement, Joseph form. Returns the log likelihood and the squared Mahalanobis distance.
template <int m> static bool kfUpdate(V5& x, M5& P, const Mat<m, 5>& H, const Mat<m, 1>& z, const Mat<m, m>& R, double& logL, double& d2) {
    Mat<m, 1> y = z - H * x;
    Mat<m, m> S = H * P * tr(H) + R, Si;
    double ld = 0;
    if (!invert<m>(S, Si, &ld)) return false;
    d2 = (tr(y) * Si * y)(0, 0);
    logL = -0.5 * (d2 + ld + m * std::log(2 * kPi));
    Mat<5, m> K = P * tr(H) * Si;
    x = x + K * y;
    M5 A = M5::eye() - K * H;
    P = A * P * tr(A) + K * R * tr(K);
    symmetrize(P);
    for (int i = 0; i < 5; i++) if (P(i, i) < 1e-12) P(i, i) = 1e-12;
    return true;
}

static void mixedH(const TrackEstimator::Core& c, V5& x, M5& P) {
    x = V5(); P = M5();
    for (int j = 0; j < 3; j++) for (int k = 0; k < 5; k++) x(k, 0) += c.hmu[j] * c.hx[j](k, 0);
    for (int j = 0; j < 3; j++) { V5 d; for (int k = 0; k < 5; k++) d(k, 0) = c.hx[j](k, 0) - x(k, 0); P = P + c.hmu[j] * (c.hP[j] + d * tr(d)); }
    symmetrize(P);
}

// IMM measurement step: update all models, re-weight them
template <int m> static bool immUpdate(TrackEstimator::Core& c, const Mat<m, 5>& H, const Mat<m, 1>& z, const Mat<m, m>& R) {
    double ll[3]; double mx = -1e300; bool ok = true;
    for (int j = 0; j < 3; j++) { double d2; if (!kfUpdate<m>(c.hx[j], c.hP[j], H, z, R, ll[j], d2)) { ok = false; ll[j] = -1e30; } mx = std::max(mx, ll[j]); }
    if (!ok && mx < -1e29) return false;
    double s = 0, mu[3];
    for (int j = 0; j < 3; j++) { mu[j] = c.hmu[j] * std::exp(std::max(-700.0, ll[j] - mx)); s += mu[j]; }
    if (!(s > 0)) return false;
    double s2 = 0;
    for (int j = 0; j < 3; j++) { mu[j] = std::max(mu[j] / s, 1e-4); s2 += mu[j]; }
    for (int j = 0; j < 3; j++) c.hmu[j] = mu[j] / s2;
    return true;
}

// ---- vertical channel --------------------------------------------------------------------------------------------------------
using V2 = Mat<2, 1>;
using M2 = Mat<2, 2>;

static void vPropagate(int j, V2& x, M2& P, double T, const TrackOptions& o, bool cov) {
    const double tau = j == 0 ? 15.0 : 120.0, a = std::exp(-T / tau), q = j == 0 ? o.qVertLevel : o.qVertClimb;
    const double g = tau * (1 - a);
    const double h = x(0, 0), v = x(1, 0);
    x(0, 0) = h + g * v; x(1, 0) = a * v;
    if (!cov) return;
    M2 F; F(0, 0) = 1; F(0, 1) = g; F(1, 1) = a;
    M2 Q; Q(0, 0) = q * T * T * T / 3; Q(0, 1) = Q(1, 0) = q * T * T / 2; Q(1, 1) = q * T;
    P = F * P * tr(F) + Q;
    symmetrize(P);
}

static void vPredictTo(TrackEstimator::Core& c, double tNew, const TrackOptions& o, bool cov) {
    const double dt = tNew - c.vt;
    if (dt <= 0) return;
    const int steps = std::max(1, (int)std::ceil(dt / o.maxStepSec));
    const double T = dt / steps;
    for (int s = 0; s < steps; s++) {
        const double leaveL = 1 - std::exp(-T / o.vertSojournSec[0]), leaveC = 1 - std::exp(-T / o.vertSojournSec[1]);
        const double Pi[2][2] = {{1 - leaveL, leaveL}, {leaveC, 1 - leaveC}};
        double cb[2] = {0, 0};
        for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) cb[j] += Pi[i][j] * c.vmu[i];
        V2 x0[2]; M2 P0[2];
        for (int j = 0; j < 2; j++) {
            const double cj = std::max(cb[j], 1e-300);
            for (int i = 0; i < 2; i++) { const double m = Pi[i][j] * c.vmu[i] / cj; for (int k = 0; k < 2; k++) x0[j](k, 0) += m * c.vx[i](k, 0); }
            if (cov) for (int i = 0; i < 2; i++) { const double m = Pi[i][j] * c.vmu[i] / cj; V2 d; for (int k = 0; k < 2; k++) d(k, 0) = c.vx[i](k, 0) - x0[j](k, 0); P0[j] = P0[j] + m * (c.vP[i] + d * tr(d)); }
        }
        for (int j = 0; j < 2; j++) { c.vx[j] = x0[j]; c.vP[j] = P0[j]; c.vmu[j] = cb[j]; vPropagate(j, c.vx[j], c.vP[j], T, o, cov); }
        const double sm = c.vmu[0] + c.vmu[1];
        c.vmu[0] /= sm; c.vmu[1] /= sm;
    }
    c.vt = tNew;
}

template <int m> static void vImmUpdate(TrackEstimator::Core& c, const Mat<m, 2>& H, const Mat<m, 1>& z, const Mat<m, m>& R) {
    double ll[2], mx = -1e300;
    for (int j = 0; j < 2; j++) {
        Mat<m, 1> y = z - H * c.vx[j];
        Mat<m, m> S = H * c.vP[j] * tr(H) + R, Si; double ld;
        if (!invert<m>(S, Si, &ld)) { ll[j] = -1e30; continue; }
        const double d2 = (tr(y) * Si * y)(0, 0);
        ll[j] = -0.5 * (d2 + ld + m * std::log(2 * kPi));
        Mat<2, m> K = c.vP[j] * tr(H) * Si;
        c.vx[j] = c.vx[j] + K * y;
        M2 A = M2::eye() - K * H;
        c.vP[j] = A * c.vP[j] * tr(A) + K * R * tr(K);
        symmetrize(c.vP[j]);
        for (int i = 0; i < 2; i++) if (c.vP[j](i, i) < 1e-9) c.vP[j](i, i) = 1e-9;
        mx = std::max(mx, ll[j]);
    }
    if (mx < -1e29) return;
    double mu[2], s = 0;
    for (int j = 0; j < 2; j++) { mu[j] = c.vmu[j] * std::exp(std::max(-700.0, ll[j] - mx)); s += mu[j]; }
    double s2 = 0;
    for (int j = 0; j < 2; j++) { mu[j] = std::max(mu[j] / s, 1e-4); s2 += mu[j]; }
    for (int j = 0; j < 2; j++) c.vmu[j] = mu[j] / s2;
}

// ---- frame moves -------------------------------------------------------------------------------------------------------------
static void rebase(TrackEstimator::Core& c, double lat, double lon) {
    Frame nf; nf.set(lat, lon);
    auto mapPoint = [&](double e, double n, double& e2, double& n2) { double la, lo; c.fr.toGeo(e, n, la, lo); nf.toLocal(la, lo, e2, n2); };
    double e0 = 0, n0 = 0;
    // Jacobian of old local -> new local at the track's position, from the mean of the models
    double me = 0, mn = 0; for (int j = 0; j < 3; j++) { me += c.hmu[j] * c.hx[j](0, 0); mn += c.hmu[j] * c.hx[j](1, 0); }
    mapPoint(me, mn, e0, n0);
    double ee, en, ne, nn;
    const double h = 50.0;
    mapPoint(me + h, mn, ee, en); mapPoint(me, mn + h, ne, nn);
    Mat<2, 2> J; J(0, 0) = (ee - e0) / h; J(1, 0) = (en - n0) / h; J(0, 1) = (ne - e0) / h; J(1, 1) = (nn - n0) / h;
    Mat<5, 5> T = Mat<5, 5>::eye();
    for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) { T(a, b) = J(a, b); T(2 + a, 2 + b) = J(a, b); }
    for (int j = 0; j < 3; j++) {
        double e2, n2; mapPoint(c.hx[j](0, 0), c.hx[j](1, 0), e2, n2);
        V5 x = c.hx[j]; V5 y = T * x; y(0, 0) = e2; y(1, 0) = n2; c.hx[j] = y;
        c.hP[j] = T * c.hP[j] * tr(T);
    }
    double e2, n2;
    mapPoint(c.lastPosE, c.lastPosN, e2, n2); c.lastPosE = e2; c.lastPosN = n2;
    for (int k = 0; k < 2; k++) { mapPoint(c.rejE[k], c.rejN[k], e2, n2); c.rejE[k] = e2; c.rejN[k] = n2; }
    c.fr = nf;
}

// Angle (radians, clockwise) from the frame's north axis to true north at a point: velocities and tracks given against the local meridian
// are turned into frame azimuths with it (frame azimuth = true azimuth + gamma). Large near the poles.
static double convergence(const Frame& fr, double lat, double lon) {
    double la2, lo2, e1, n1, e2, n2;
    gcDestination(lat, lon, 0, 2000.0, la2, lo2);
    fr.toLocal(lat, lon, e1, n1); fr.toLocal(la2, lo2, e2, n2);
    return std::atan2(e2 - e1, n2 - n1);
}


// ---- the estimator -----------------------------------------------------------------------------------------------------------
TrackEstimator::TrackEstimator(const TrackOptions& o) : o_(o) {}

void TrackEstimator::setWindPrior(double e, double n, double sigma) {
    if (c_.windVar > sigma * sigma && c_.windT < -1e17) { c_.windE = e; c_.windN = n; c_.windVar = sigma * sigma; }
}

static void initTrack(TrackEstimator::Core& c, const Fix& f, const TrackOptions& o, double posSig) {
    const double lastWindE = c.windE, lastWindN = c.windN, lastWindVar = c.windVar, lastWindT = c.windT;
    const double lastNis = c.nis, lastQs = c.qScale;
    c = TrackEstimator::Core();
    c.nis = lastNis; c.qScale = lastQs;
    c.windE = lastWindE; c.windN = lastWindN; c.windVar = lastWindVar; c.windT = lastWindT;
    c.fr.set(f.lat, f.lon);
    c.init = true; c.t = f.t;
    double ve = 0, vn = 0, vs2 = o.initSpeedSigmaMs * o.initSpeedSigmaMs;
    if (f.hasVel && !f.velIsAir) {
        const double s = f.speedMs, th = f.trackDeg * kDeg, sg = f.speedSigmaMs > 0 ? f.speedSigmaMs : 3.0;
        ve = s * std::sin(th); vn = s * std::cos(th); vs2 = sg * sg + s * s * 0.03 * 0.03;
    } else if (f.t - c.pendT < 600 && c.pendT > -1e17) { ve = c.pendSpeed * std::sin(c.pendTrack * kDeg); vn = c.pendSpeed * std::cos(c.pendTrack * kDeg); vs2 = 25; }
    for (int j = 0; j < 3; j++) {
        c.hx[j](2, 0) = ve; c.hx[j](3, 0) = vn;
        c.hP[j](0, 0) = c.hP[j](1, 1) = posSig * posSig;
        c.hP[j](2, 2) = c.hP[j](3, 3) = vs2;
        c.hP[j](4, 4) = j == kCT ? 0.02 * 0.02 : 1e-8;
    }
    c.hmu[0] = 0.8; c.hmu[1] = 0.1; c.hmu[2] = 0.1;
    c.lastPosT = f.t; c.lastPosLat = f.lat; c.lastPosLon = f.lon; c.lastPosE = c.lastPosN = 0; c.lastSrc = f.src;
    c.srcW[(int)f.src] = 1; c.nPos = 1;
}

UpdateResult TrackEstimator::update(const Fix& f0) {
    if (!(std::isfinite(f0.t))) return UpdateResult::RejectedLate;
    Fix f = f0;
    f.t -= f.latencySec >= 0 ? f.latencySec : o_.latencySec[(int)f.src];      // the filter works on the time the position is valid for
    f.latencySec = 0;
    // late fix: re-fit from the snapshot before the first fix that is newer
    if (c_.init && f.t < c_.t - 1e-6) {
        if (c_.t - f.t > o_.retroWindowSec) return UpdateResult::RejectedLate;
        size_t k = hist_.size();
        for (size_t i = 0; i < hist_.size(); i++) if (hist_[i].fix.t > f.t) { k = i; break; }
        if (k >= hist_.size()) return UpdateResult::RejectedLate;
        std::vector<Fix> replay; replay.push_back(f);
        for (size_t i = k; i < hist_.size(); i++) replay.push_back(hist_[i].fix);
        c_ = hist_[k].core;
        hist_.erase(hist_.begin() + (long)k, hist_.end());
        UpdateResult r = UpdateResult::Retrofitted;
        for (const Fix& g : replay) {
            Snap s; s.core = c_; s.fix = g;
            const UpdateResult ur = process(g);
            if (ur != UpdateResult::RejectedLate) { hist_.push_back(s); if ((int)hist_.size() > o_.retroDepth) hist_.pop_front(); }
            (void)ur;
        }
        return r;
    }
    Snap s; s.core = c_; s.fix = f;
    const UpdateResult r = process(f);
    if (r != UpdateResult::RejectedLate) { hist_.push_back(s); if ((int)hist_.size() > o_.retroDepth) hist_.pop_front(); }
    return r;
}

UpdateResult TrackEstimator::process(const Fix& f) {
    Core& c = c_;
    UpdateResult res = UpdateResult::Accepted;
    // wind reports: smoothed toward the track's own wind
    if (f.hasWind && std::isfinite(f.windMs)) {
        const double we = -f.windMs * std::sin(f.windFromDeg * kDeg), wn = -f.windMs * std::cos(f.windFromDeg * kDeg), r = 4.0 * 4.0;
        if (c.windT < -1e17) { c.windE = we; c.windN = wn; c.windVar = r; }
        else { c.windVar += 0.02 * std::max(0.0, f.t - c.windT); const double k = c.windVar / (c.windVar + r); c.windE += k * (we - c.windE); c.windN += k * (wn - c.windN); c.windVar *= (1 - k); }
        c.windT = f.t;
    }
    if (!f.hasPos) {
        if (f.hasVel && !f.velIsAir && !c.init) { c.pendSpeed = f.speedMs; c.pendTrack = f.trackDeg; c.pendT = f.t; }
        if (!c.init) { res = UpdateResult::NoPosition; }
    }
    if (f.hasPos && !(std::isfinite(f.lat) && std::isfinite(f.lon) && std::fabs(f.lat) <= 90)) return UpdateResult::RejectedLate;
    // ---- horizontal
    if (f.hasPos) {
        double speedNow = 230;
        if (c.init) { V5 x; M5 P; mixedH(c, x, P); speedNow = std::hypot(x(2, 0), x(3, 0)); }
        const double sp = fixPosSigmaM(f, speedNow);
        bool restart = !c.init;
        if (c.init && f.t - c.lastPosT > o_.maxGapSec) restart = true;
        if (restart) { initTrack(c, f, o_, sp); res = res == UpdateResult::NoPosition ? UpdateResult::Started : UpdateResult::Started; }
        else {
            // time update, then the gate on the mixed prediction
            predictTo(c, f.t, o_, true);
            double e, n; c.fr.toLocal(f.lat, f.lon, e, n);
            V5 xm; M5 Pm; mixedH(c, xm, Pm);
            const double dx = e - xm(0, 0), dy = n - xm(1, 0);
            const double sxx = Pm(0, 0) + sp * sp, syy = Pm(1, 1) + sp * sp, sxy = Pm(0, 1);
            const double det = sxx * syy - sxy * sxy;
            const double d2 = det > 0 ? (dx * dx * syy - 2 * dx * dy * sxy + dy * dy * sxx) / det : 1e30;
            // implied speed from the last accepted position
            const double dtl = f.t - c.lastPosT;
            const double jump = std::hypot(e - c.lastPosE, n - c.lastPosN);
            const bool fast = dtl > 0.2 && (jump - 4 * (sp + std::sqrt(std::max(Pm(0, 0), 0.0)))) / dtl > o_.maxSpeedMs * 1.3;
            const double gateNow = o_.gateChi2 * (1.0 + std::pow(std::min(dtl, 3000.0) / 30.0, 2));   // sparse sources: the model, not the report, is the likely culprit
            if (d2 > gateNow || fast) {
                c.nis = 0.85 * c.nis + 0.15 * (std::min(d2, 4 * o_.gateChi2) / 2.0);      // a rejected fix is still evidence that the model is too stiff
                c.qScale = clampd(c.nis, 1.0, o_.nisMax);
                // keep it for the restart test
                c.rejE[0] = c.rejE[1]; c.rejN[0] = c.rejN[1]; c.rejT[0] = c.rejT[1];
                c.rejE[1] = e; c.rejN[1] = n; c.rejT[1] = f.t;
                const double dtr = c.rejT[1] - c.rejT[0];
                const bool consistent = c.rejectRun >= 1 && std::hypot(c.rejE[1] - c.rejE[0], c.rejN[1] - c.rejN[0]) < 6 * sp + 300.0 * std::max(0.0, dtr);
                c.rejectRun = consistent || c.rejectRun == 0 ? c.rejectRun + 1 : 1;
                if (c.rejectRun >= o_.reinitAfter) { initTrack(c, f, o_, sp); res = UpdateResult::Restarted; }
                else { nRejected_++; return fast ? UpdateResult::RejectedSpeed : UpdateResult::RejectedGate; }
            } else {
                c.rejectRun = 0;
                c.nis = 0.85 * c.nis + 0.15 * (d2 / 2.0);
                c.qScale = clampd(c.nis, 1.0, o_.nisMax);
                Mat<2, 5> H; H(0, 0) = 1; H(1, 1) = 1;
                Mat<2, 1> z; z(0, 0) = e; z(1, 0) = n;
                Mat<2, 2> R; R(0, 0) = R(1, 1) = sp * sp;
                if (!immUpdate<2>(c, H, z, R)) { nRejected_++; return UpdateResult::RejectedGate; }
                c.lastPosE = e; c.lastPosN = n; c.lastPosLat = f.lat; c.lastPosLon = f.lon;
                if (c.nPos > 0) { const double gap = f.t - c.lastPosT; c.interval = c.interval == 0 ? gap : 0.8 * c.interval + 0.2 * gap; }
                c.lastPosT = f.t; c.lastSrc = f.src; c.nPos++;
                for (int s = 0; s < kSrcCount; s++) c.srcW[s] *= 0.85;
                c.srcW[(int)f.src] += 0.15;
                // keep the frame near the track
                if (std::hypot(e, n) > o_.rebaseM) rebase(c, f.lat, f.lon);
            }
        }
    }
    if (c.init) {
        // velocity
        if (f.hasVel && std::isfinite(f.speedMs) && f.speedMs >= 0) {
            predictTo(c, f.t, o_, true);
            const double gam = convergence(c.fr, f.hasPos ? f.lat : c.lastPosLat, f.hasPos ? f.lon : c.lastPosLon);
            double s = f.speedMs, th = f.trackDeg * kDeg + gam;
            double wvar = 0, we = 0, wn = 0;
            if (f.velIsAir) { we = c.windE * std::cos(gam) + c.windN * std::sin(gam); wn = -c.windE * std::sin(gam) + c.windN * std::cos(gam); wvar = c.windVar + 9.0; }
            const double sg = f.speedSigmaMs > 0 ? f.speedSigmaMs : (f.src == Src::Adsb || f.src == Src::Uat ? 1.2 : 4.0);
            const double tg = (f.trackSigmaDeg > 0 ? f.trackSigmaDeg : (f.src == Src::Adsb || f.src == Src::Uat ? 1.5 : 4.0)) * kDeg;
            Mat<2, 1> z; z(0, 0) = s * std::sin(th) + we; z(1, 0) = s * std::cos(th) + wn;
            Mat<2, 2> R;
            const double sn = std::sin(th), cs = std::cos(th);
            R(0, 0) = sn * sn * sg * sg + s * s * cs * cs * tg * tg + wvar; R(1, 1) = cs * cs * sg * sg + s * s * sn * sn * tg * tg + wvar;
            R(0, 1) = R(1, 0) = sn * cs * sg * sg - s * s * sn * cs * tg * tg;
            Mat<2, 5> H; H(0, 2) = 1; H(1, 3) = 1;
            V5 xm; M5 Pm; mixedH(c, xm, Pm);
            // gate: a velocity that disagrees wildly with the track is ignored (wrong field, other aircraft)
            const double dvx = z(0, 0) - xm(2, 0), dvy = z(1, 0) - xm(3, 0);
            const double sxx = Pm(2, 2) + R(0, 0), syy = Pm(3, 3) + R(1, 1), sxy = Pm(2, 3) + R(0, 1), det = sxx * syy - sxy * sxy;
            const double d2 = det > 0 ? (dvx * dvx * syy - 2 * dvx * dvy * sxy + dvy * dvy * sxx) / det : 1e30;
            if (!c.hasVelEver || d2 < 30.0 || f.t - c.lastPosT > 600) { immUpdate<2>(c, H, z, R); c.hasVelEver = true; }
        }
        if (f.hasRoll && std::isfinite(f.rollDeg)) {
            predictTo(c, f.t, o_, true);
            V5 xm; M5 Pm; mixedH(c, xm, Pm);
            const double v = std::max(60.0, std::hypot(xm(2, 0), xm(3, 0)));
            const double roll = clampd(f.rollDeg, -60, 60) * kDeg;
            const double w = -9.80665 * std::tan(roll) / v;          // clockwise (right bank) is negative in the math sense
            Mat<1, 5> H; H(0, 4) = 1; Mat<1, 1> z; z(0, 0) = w; Mat<1, 1> R; R(0, 0) = std::pow(0.004 + 0.15 * std::fabs(w), 2);
            immUpdate<1>(c, H, z, R);
        }
    }
    // ---- vertical
    if (f.hasAlt && std::isfinite(f.altM)) {
        const double sg = f.altSigmaM > 0 ? f.altSigmaM : (f.src == Src::Adsb || f.src == Src::Uat ? 8.0 : 30.0);
        if (!c.vinit) {
            c.vinit = true; c.vt = f.t;
            for (int j = 0; j < 2; j++) { c.vx[j](0, 0) = f.altM; c.vx[j](1, 0) = 0; c.vP[j](0, 0) = sg * sg; c.vP[j](1, 1) = 10.0 * 10.0; }
        } else if (f.t >= c.vt) {
            vPredictTo(c, f.t, o_, true);
            // gate on a spike
            double hm = 0, vv = 0; for (int j = 0; j < 2; j++) { hm += c.vmu[j] * c.vx[j](0, 0); vv += c.vmu[j] * c.vP[j](0, 0); }
            double hv = 0; for (int j = 0; j < 2; j++) hv += c.vmu[j] * (c.vP[j](0, 0) + std::pow(c.vx[j](0, 0) - hm, 2));
            const double d2 = std::pow(f.altM - hm, 2) / (hv + sg * sg);
            (void)vv;
            if (d2 < 36.0) { Mat<1, 2> H; H(0, 0) = 1; Mat<1, 1> z; z(0, 0) = f.altM; Mat<1, 1> R; R(0, 0) = sg * sg; vImmUpdate<1>(c, H, z, R); }
        }
    }
    if (f.hasVrate && c.vinit && std::isfinite(f.vrateMs)) {
        vPredictTo(c, std::max(f.t, c.vt), o_, true);
        const double sg = f.vrateSigmaMs > 0 ? f.vrateSigmaMs : (f.src == Src::Adsb || f.src == Src::Uat ? 1.0 : 2.5);
        Mat<1, 2> H; H(0, 1) = 1; Mat<1, 1> z; z(0, 0) = f.vrateMs; Mat<1, 1> R; R(0, 0) = sg * sg;
        vImmUpdate<1>(c, H, z, R);
    }
    if (res == UpdateResult::NoPosition && !f.hasPos && (f.hasAlt || f.hasVrate)) res = UpdateResult::Accepted;
    if (res == UpdateResult::Accepted || res == UpdateResult::Started || res == UpdateResult::Restarted) nAccepted_++;
    return res;
}

// ---- output ------------------------------------------------------------------------------------------------------------------
static void ellipseOf(double a, double b, double cc, Ellipse& el) {   // covariance [[a,b],[b,cc]]
    const double m = 0.5 * (a + cc), d = std::sqrt(std::max(0.0, 0.25 * (a - cc) * (a - cc) + b * b));
    const double l1 = std::max(0.0, m + d), l2 = std::max(0.0, m - d), k = std::sqrt(5.991);
    el.majorM = k * std::sqrt(l1); el.minorM = k * std::sqrt(l2);
    const double th = 0.5 * std::atan2(2 * b, a - cc);            // from east, counter-clockwise
    el.orientDeg = wrap360(90.0 - th / kDeg);
    if (el.orientDeg >= 180) el.orientDeg -= 180;
}

bool TrackEstimator::estimate(double now, PosEstimate& o, bool quick) const {
    o = PosEstimate();
    const Core& s = c_;
    if (!s.init) return false;
    Core c = s;                                   // the filter itself is not moved: display times can go back and forth
    const double tn = std::max(now, s.t);
    predictTo(c, tn, o_, !quick);
    if (s.vinit) vPredictTo(c, std::max(tn, s.vt), o_, !quick);
    V5 x; M5 P;
    mixedH(c, x, P);
    const double age = std::max(0.0, now - s.lastPosT);

    // ---- route prior (only with a covariance, and when the position is not a fresh fix)
    // The aircraft is assumed to fly the great circle from its last fix to the destination while its track points at the destination (within
    // routeMaxHeadingErrDeg). The cross-track distance from that line and the cross-track speed are measurements of zero with a sigma that grows
    // with the distance flown since the fix (airways and waypoints make the real path wander); past the destination the aircraft is parked there.
    bool routeUsed = false, arrived = false; double etaT = 0;
    if (!quick && route_.hasDest && age > 5.0) {
        const double sp = std::hypot(x(2, 0), x(3, 0));
        const double distLast = gcDistM(s.lastPosLat, s.lastPosLon, route_.destLat, route_.destLon);
        const double brgLast = gcBearingDeg(s.lastPosLat, s.lastPosLon, route_.destLat, route_.destLon);
        double laN, loN; c.fr.toGeo(x(0, 0), x(1, 0), laN, loN);
        const double gamN = convergence(c.fr, laN, loN);
        const double trk = wrap360(std::atan2(x(2, 0), x(3, 0)) / kDeg - gamN / kDeg);
        if (sp > 40 && std::fabs(wrap180(trk - brgLast)) < o_.routeMaxHeadingErrDeg) {
            double la, lo; c.fr.toGeo(x(0, 0), x(1, 0), la, lo);
            const double flown = gcDistM(s.lastPosLat, s.lastPosLon, la, lo);
            if (flown >= distLast - 2000 || distLast < 3000) {
                double ex, nx; c.fr.toLocal(route_.destLat, route_.destLon, ex, nx);
                x(0, 0) = ex; x(1, 0) = nx; x(2, 0) = x(3, 0) = 0;
                P(0, 0) = std::min(P(0, 0), 4e6); P(1, 1) = std::min(P(1, 1), 4e6); P(0, 1) = P(1, 0) = 0;
                arrived = true; etaT = now; routeUsed = true;
            } else {
                double xt, at; gcTrackXy(s.lastPosLat, s.lastPosLon, route_.destLat, route_.destLon, la, lo, xt, at);
                const double remainNow = std::max(0.0, distLast - flown);
                const double sx = o_.routeXtBaseM + o_.routeXtFrac * flown + o_.routeXtRemFrac * remainNow, sv = sp * (o_.routeHeadingFrac + 0.2 * std::min(1.0, remainNow / 500e3));
                const double b = gcBearingDeg(la, lo, route_.destLat, route_.destLon) * kDeg + convergence(c.fr, la, lo);
                const double nr_e = std::cos(b), nr_n = -std::sin(b), u_e = std::sin(b), u_n = std::cos(b);
                Mat<2, 5> H; H(0, 0) = nr_e; H(0, 1) = nr_n; H(1, 2) = nr_e; H(1, 3) = nr_n;
                Mat<2, 1> z; z(0, 0) = -xt + (nr_e * x(0, 0) + nr_n * x(1, 0)); z(1, 0) = 0;
                Mat<2, 2> R; R(0, 0) = sx * sx; R(1, 1) = sv * sv;
                double ll, d2;
                kfUpdate<2>(x, P, H, z, R, ll, d2);
                const double remain = std::max(0.0, distLast - flown);
                if (route_.etaT > now && route_.etaT > s.lastPosT + 60) {
                    const double vEta = remain / (route_.etaT - now);
                    if (vEta > 90 && vEta < 320) {
                        Mat<1, 5> Hu; Hu(0, 2) = u_e; Hu(0, 3) = u_n; Mat<1, 1> zu; zu(0, 0) = vEta; Mat<1, 1> Ru; Ru(0, 0) = 30.0 * 30.0;
                        kfUpdate<1>(x, P, Hu, zu, Ru, ll, d2);
                    }
                }
                routeUsed = true;
                const double along = x(2, 0) * u_e + x(3, 0) * u_n;
                etaT = now + remain / std::max(60.0, along);
            }
        }
    }

    c.fr.toGeo(x(0, 0), x(1, 0), o.lat, o.lon);
    const double gam = convergence(c.fr, o.lat, o.lon), cg = std::cos(gam), sg = std::sin(gam);
    const double sp = std::hypot(x(2, 0), x(3, 0));
    o.speedKt = sp / kKt;
    o.trackDeg = sp > 1.0 ? wrap360(std::atan2(x(2, 0), x(3, 0)) / kDeg - gam / kDeg) : 0;
    o.turnRateDegS = -x(4, 0) / kDeg * (c.hmu[kCT]);   // CT model share: the other models carry no turn rate
    {   // covariance in true east/north at the estimate: rotate out the frame's convergence
        const double a = P(0, 0), b = P(0, 1), d = P(1, 1);
        o.cEE = cg * cg * a - 2 * cg * sg * b + sg * sg * d;
        o.cNN = sg * sg * a + 2 * cg * sg * b + cg * cg * d;
        o.cEN = cg * sg * (a - d) + (cg * cg - sg * sg) * b;
    }
    o.sigmaH = quick ? 0 : std::sqrt(0.5 * (P(0, 0) + P(1, 1)));
    if (!quick) ellipseOf(o.cEE, o.cEN, o.cNN, o.ellipse);
    for (int j = 0; j < 3; j++) o.modelProb[j] = c.hmu[j];
    if (s.vinit) {
        double h = 0, v = 0, hv = 0;
        for (int j = 0; j < 2; j++) { h += c.vmu[j] * c.vx[j](0, 0); v += c.vmu[j] * c.vx[j](1, 0); }
        for (int j = 0; j < 2; j++) hv += c.vmu[j] * (c.vP[j](0, 0) + std::pow(c.vx[j](0, 0) - h, 2));
        o.altM = h; o.altFt = h / kFt; o.vrateFpm = v / kFpm; o.sigmaV = quick ? 0 : std::sqrt(std::max(0.0, hv));
        o.vertClimbing = c.vmu[1] > 0.5;
    }
    double tot = 0; for (int i = 0; i < kSrcCount; i++) tot += s.srcW[i];
    int best = 0;
    for (int i = 0; i < kSrcCount; i++) { o.srcWeight[i] = tot > 0 ? s.srcW[i] / tot : 0; if (s.srcW[i] > s.srcW[best]) best = i; }
    o.mainSrc = (Src)best;
    o.ageSec = age; o.posAgeSec = age;
    const bool sat = s.lastSrc == Src::Acars || s.lastSrc == Src::Vdl2 || s.lastSrc == Src::Hfdl;
    o.predicted = age > (sat ? o_.predictedAfterSatSec : o_.predictedAfterSec);
    o.interpolated = !o.predicted && age > 0.5;
    if (!quick) {
        const double r95 = std::max(o.ellipse.majorM, 30.0);
        o.confidence = clampd(1.0 - std::log10(r95 / 50.0) / std::log10(300000.0 / 50.0), 0, 1);
    }
    o.staleness = clampd(age / 3600.0, 0, 1);
    o.routeUsed = routeUsed; o.arrived = arrived; o.etaT = etaT;
    o.windE = s.windE; o.windN = s.windN;      // true east/north
    o.valid = std::isfinite(o.lat) && std::isfinite(o.lon);
    return o.valid;
}

}  // namespace dect2::track
