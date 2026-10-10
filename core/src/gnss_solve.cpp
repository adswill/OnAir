// Pseudorange position solution (see gnss_solve.h).
#include "dect2/gnss_solve.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {

// solves A x = b for a symmetric positive definite n x n matrix (Cholesky); returns false when singular. A is replaced by its inverse when `inverse` is set.
bool cholSolve(double* A, double* b, int n, double* inv) {
    double L[81] = {};
    for (int i = 0; i < n; i++)
        for (int j = 0; j <= i; j++) {
            double s = A[i * n + j];
            for (int k = 0; k < j; k++) s -= L[i * n + k] * L[j * n + k];
            if (i == j) { if (s <= 1e-12) return false; L[i * n + i] = std::sqrt(s); }
            else L[i * n + j] = s / L[j * n + j];
        }
    if (b) {
        double y[9];
        for (int i = 0; i < n; i++) { double s = b[i]; for (int k = 0; k < i; k++) s -= L[i * n + k] * y[k]; y[i] = s / L[i * n + i]; }
        for (int i = n - 1; i >= 0; i--) { double s = y[i]; for (int k = i + 1; k < n; k++) s -= L[k * n + i] * b[k]; b[i] = s / L[i * n + i]; }
    }
    if (inv) {
        for (int c = 0; c < n; c++) {
            double y[9], x[9];
            for (int i = 0; i < n; i++) { double s = (i == c) ? 1.0 : 0.0; for (int k = 0; k < i; k++) s -= L[i * n + k] * y[k]; y[i] = s / L[i * n + i]; }
            for (int i = n - 1; i >= 0; i--) { double s = y[i]; for (int k = i + 1; k < n; k++) s -= L[k * n + i] * x[k]; x[i] = s / L[i * n + i]; }
            for (int i = 0; i < n; i++) inv[i * n + c] = x[i];
        }
    }
    return true;
}

// chi-square quantile for a false alarm of 0.001 (Wilson-Hilferty)
double chi2Limit(int dof) {
    const double k = dof, z = 3.09, a = 2.0 / (9.0 * k);
    const double t = 1.0 - a + z * std::sqrt(a);
    return k * t * t * t;
}

} // namespace

namespace {

struct Core {
    bool ok = false;
    double x[3] = {0, 0, 0};
    double bias[GnssSystems] = {0, 0, 0, 0};
    int col[GnssSystems] = {-1, -1, -1, -1, -1, -1};
    int nUse = 0, nUnk = 0, iterations = 0;
    double sw = 0, sw2 = 0;               // weighted and plain sum of squared residuals
    std::vector<double> res, w, el, az, iono, tropo;
};

// One weighted least squares solution of the satellites marked in `use`; satellites of a system with fewer than two of them are cleared from `use`.
Core solveSet(const std::vector<GnssObs>& obs, const GnssSolveOptions& opt, std::vector<char>& use, const double* start) {
    Core c;
    const size_t N = obs.size();
    int perSys[GnssSystems] = {0, 0, 0, 0};
    for (size_t i = 0; i < N; i++) if (use[i]) perSys[obs[i].sys]++;
    int nSys = 0;
    for (int s = 0; s < GnssSystems; s++) if (perSys[s] >= 2) c.col[s] = nSys++;
    for (size_t i = 0; i < N; i++) if (use[i] && c.col[obs[i].sys] < 0) use[i] = 0;
    c.nUse = 0;
    for (size_t i = 0; i < N; i++) c.nUse += use[i];
    c.nUnk = 3 + nSys;
    if (nSys == 0 || c.nUse < c.nUnk) return c;
    c.res.assign(N, 0.0); c.w.assign(N, 0.0); c.el.assign(N, 90.0); c.az.assign(N, 0.0); c.iono.assign(N, 0.0); c.tropo.assign(N, 0.0);
    double x[3] = {start[0], start[1], start[2]};
    double bias[GnssSystems] = {0, 0, 0, 0};
    bool conv = false;
    int it = 0;
    const int nUnk = c.nUnk;
    for (; it < 20; it++) {
        double A[81] = {}, b[9] = {};
        const bool haveRx = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]) > 6.0e6;
        double lat = 0, lon = 0, hgt = 0;
        if (haveRx) ecefToLla(x, &lat, &lon, &hgt);
        for (size_t i = 0; i < N; i++) {
            if (!use[i]) continue;
            double rng = std::sqrt((obs[i].sat[0] - x[0]) * (obs[i].sat[0] - x[0]) + (obs[i].sat[1] - x[1]) * (obs[i].sat[1] - x[1]) + (obs[i].sat[2] - x[2]) * (obs[i].sat[2] - x[2]));
            const double tau = haveRx ? rng / kC : 0.075;
            const double a = kEarthRate * tau, ca = std::cos(a), sa = std::sin(a);
            const double sx = obs[i].sat[0] * ca + obs[i].sat[1] * sa, sy = -obs[i].sat[0] * sa + obs[i].sat[1] * ca, sz = obs[i].sat[2];
            const double dx = sx - x[0], dy = sy - x[1], dz = sz - x[2];
            rng = std::sqrt(dx * dx + dy * dy + dz * dz);
            double ion = 0, trp = 0;
            if (haveRx) {
                const double s3[3] = {sx, sy, sz};
                azElFromEcef(x, s3, &c.az[i], &c.el[i]);
                if (c.el[i] > 0.5) {
                    if (opt.hasIono) ion = kC * klobucharDelay(opt.iono, lat, lon, c.az[i], c.el[i], opt.tow);
                    if (opt.tropo) trp = troposphereDelay(lat, hgt, c.el[i]);
                }
            }
            c.iono[i] = ion; c.tropo[i] = trp;
            const double y = (obs[i].pr - ion - trp) - (rng + bias[obs[i].sys]);
            c.res[i] = y;
            const double se = std::sin(std::max(c.el[i], 3.0) * kPi / 180.0);
            c.w[i] = se * se * obs[i].weight;
            double h[9] = {};
            h[0] = -dx / rng; h[1] = -dy / rng; h[2] = -dz / rng;
            h[3 + c.col[obs[i].sys]] = 1.0;
            for (int r = 0; r < nUnk; r++) {
                b[r] += c.w[i] * h[r] * y;
                for (int cc = 0; cc < nUnk; cc++) A[r * nUnk + cc] += c.w[i] * h[r] * h[cc];
            }
        }
        double d[9];
        for (int r = 0; r < nUnk; r++) d[r] = b[r];
        if (!cholSolve(A, d, nUnk, nullptr)) return c;
        for (int k = 0; k < 3; k++) x[k] += d[k];
        for (int s = 0; s < GnssSystems; s++) if (c.col[s] >= 0) bias[s] += d[3 + c.col[s]];
        if (std::fabs(d[0]) + std::fabs(d[1]) + std::fabs(d[2]) < 1e-5 && it >= 3) { conv = true; it++; break; }
    }
    if (!conv) return c;
    // residuals at the solution
    double lat, lon, hgt;
    ecefToLla(x, &lat, &lon, &hgt);
    for (size_t i = 0; i < N; i++) {
        if (!use[i]) continue;
        double rng = std::sqrt((obs[i].sat[0] - x[0]) * (obs[i].sat[0] - x[0]) + (obs[i].sat[1] - x[1]) * (obs[i].sat[1] - x[1]) + (obs[i].sat[2] - x[2]) * (obs[i].sat[2] - x[2]));
        const double a = kEarthRate * rng / kC;
        const double ca = std::cos(a), sa = std::sin(a);
        const double s3[3] = {obs[i].sat[0] * ca + obs[i].sat[1] * sa, -obs[i].sat[0] * sa + obs[i].sat[1] * ca, obs[i].sat[2]};
        azElFromEcef(x, s3, &c.az[i], &c.el[i]);
        rng = std::sqrt((s3[0] - x[0]) * (s3[0] - x[0]) + (s3[1] - x[1]) * (s3[1] - x[1]) + (s3[2] - x[2]) * (s3[2] - x[2]));
        double ion = 0, trp = 0;
        if (c.el[i] > 0.5) {
            if (opt.hasIono) ion = kC * klobucharDelay(opt.iono, lat, lon, c.az[i], c.el[i], opt.tow);
            if (opt.tropo) trp = troposphereDelay(lat, hgt, c.el[i]);
        }
        c.iono[i] = ion; c.tropo[i] = trp;
        c.res[i] = (obs[i].pr - ion - trp) - (rng + bias[obs[i].sys]);
        const double se = std::sin(std::max(c.el[i], 3.0) * kPi / 180.0);
        c.w[i] = se * se * obs[i].weight;
        c.sw += c.w[i] * c.res[i] * c.res[i];
        c.sw2 += c.res[i] * c.res[i];
    }
    for (int k = 0; k < 3; k++) c.x[k] = x[k];
    for (int s = 0; s < GnssSystems; s++) c.bias[s] = bias[s];
    c.iterations = it;
    c.ok = true;
    return c;
}

} // namespace

bool gnssSolve(std::vector<GnssObs>& obs, const GnssSolveOptions& opt, GnssSolution& out) {
    out = GnssSolution();
    const size_t N = obs.size();
    out.used.assign(N, 0);
    out.residual.assign(N, 0.0);
    out.elDeg.assign(N, 0.0);
    out.azDeg.assign(N, 0.0);
    if (N < 4) return false;
    double start[3] = {0, 0, 0};
    if (opt.hasPrior) for (int k = 0; k < 3; k++) start[k] = opt.prior[k];
    std::vector<char> use(N, 1);
    Core c = solveSet(obs, opt, use, start);
    if (!c.ok) return false;
    // elevation mask from the solution's own geometry
    if (opt.maskDeg > 0) {
        bool dropped = false;
        std::vector<char> u2 = use;
        int left = c.nUse;
        for (size_t i = 0; i < N; i++) if (u2[i] && c.el[i] < opt.maskDeg && left > c.nUnk) { u2[i] = 0; left--; dropped = true; }
        if (dropped) { Core c2 = solveSet(obs, opt, u2, c.x); if (c2.ok) { c = std::move(c2); use = u2; } }
    }
    // residual check: while the weighted residuals are too large for the expected pseudorange noise, drop the satellite whose removal helps most
    while (opt.outlierCheck) {
        const int dof = c.nUse - c.nUnk;
        if (dof < 1) break;
        const double T = c.sw / (opt.sigmaNominal * opt.sigmaNominal);
        if (T <= chi2Limit(dof)) break;
        if (c.nUse - 1 < c.nUnk) break;
        double bestT = 1e300;
        size_t bestI = N;
        Core bestC;
        for (size_t i = 0; i < N; i++) {
            if (!use[i]) continue;
            std::vector<char> u2 = use;
            u2[i] = 0;
            Core c2 = solveSet(obs, opt, u2, c.x);
            if (!c2.ok || c2.nUse - c2.nUnk < 0) continue;
            const int d2 = c2.nUse - c2.nUnk;
            // with no redundancy left the residuals are all zero: accept it only if it is the last way out
            const double T2 = d2 >= 1 ? c2.sw / (opt.sigmaNominal * opt.sigmaNominal) / chi2Limit(d2) : 0.5;
            if (T2 < bestT) { bestT = T2; bestI = i; bestC = std::move(c2); }
        }
        if (bestI == N || bestT >= T / chi2Limit(dof)) break;
        use[bestI] = 0;
        c = std::move(bestC);
        // the set may have shrunk further (a system left with one satellite)
        for (size_t i = 0; i < N; i++) if (use[i] && c.col[obs[i].sys] < 0) use[i] = 0;
        out.excluded++;
    }
    out.ok = true;
    for (int k = 0; k < 3; k++) out.x[k] = c.x[k];
    for (int s = 0; s < GnssSystems; s++) { out.bias[s] = c.bias[s]; out.sysUsed[s] = c.col[s] >= 0; }
    out.nUsed = c.nUse; out.nUnknowns = c.nUnk; out.iterations = c.iterations;
    int nUsedCount = 0;
    for (size_t i = 0; i < N; i++) {
        out.used[i] = use[i];
        out.residual[i] = use[i] ? c.res[i] : 0.0;
        out.elDeg[i] = c.el[i]; out.azDeg[i] = c.az[i];
        obs[i].iono = c.iono[i]; obs[i].tropo = c.tropo[i];
        nUsedCount += use[i];
    }
    out.nUsed = nUsedCount;
    const int dof = c.nUse - c.nUnk;
    out.rmsResidual = std::sqrt(c.sw2 / std::max(c.nUse, 1));
    out.sigmaUere = dof >= 1 ? std::max(1.5, std::sqrt(c.sw / dof)) : opt.sigmaNominal;
    // DOP from the unweighted geometry
    const int nUnk = c.nUnk;
    double G[81] = {}, Gw[81] = {}, cov[81], covW[81];
    for (size_t i = 0; i < N; i++) {
        if (!use[i]) continue;
        const double a = kEarthRate * 0.075, ca = std::cos(a), sa = std::sin(a);
        const double sx = obs[i].sat[0] * ca + obs[i].sat[1] * sa, sy = -obs[i].sat[0] * sa + obs[i].sat[1] * ca, sz = obs[i].sat[2];
        const double dx = sx - c.x[0], dy = sy - c.x[1], dz = sz - c.x[2];
        const double r = std::sqrt(dx * dx + dy * dy + dz * dz);
        double h[9] = {};
        h[0] = -dx / r; h[1] = -dy / r; h[2] = -dz / r; h[3 + c.col[obs[i].sys]] = 1.0;
        for (int rr = 0; rr < nUnk; rr++) for (int cc = 0; cc < nUnk; cc++) { G[rr * nUnk + cc] += h[rr] * h[cc]; Gw[rr * nUnk + cc] += c.w[i] * h[rr] * h[cc]; }
    }
    const bool haveW = cholSolve(Gw, nullptr, nUnk, covW);
    if (cholSolve(G, nullptr, nUnk, cov)) {
        double lat, lon, hh;
        ecefToLla(c.x, &lat, &lon, &hh);
        const double la = lat * kPi / 180.0, lo = lon * kPi / 180.0;
        const double R[3][3] = {{-std::sin(lo), std::cos(lo), 0}, {-std::sin(la) * std::cos(lo), -std::sin(la) * std::sin(lo), std::cos(la)}, {std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la)}};
        double Q[3][3] = {};
        for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) { double s = 0; for (int k = 0; k < 3; k++) for (int d2 = 0; d2 < 3; d2++) s += R[a][k] * cov[k * nUnk + d2] * R[b][d2]; Q[a][b] = s; }
        out.hdop = (float)std::sqrt(std::max(0.0, Q[0][0] + Q[1][1]));
        out.vdop = (float)std::sqrt(std::max(0.0, Q[2][2]));
        out.pdop = (float)std::sqrt(std::max(0.0, Q[0][0] + Q[1][1] + Q[2][2]));
        double t = 0;
        for (int s = 0; s < nUnk - 3; s++) t += cov[(3 + s) * nUnk + 3 + s];
        out.tdop = (float)std::sqrt(std::max(0.0, t));
        if (haveW) {
            double Qw[3][3] = {};
            for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) { double s2 = 0; for (int k = 0; k < 3; k++) for (int d2 = 0; d2 < 3; d2++) s2 += R[a][k] * covW[k * nUnk + d2] * R[b][d2]; Qw[a][b] = s2; }
            out.hErrM = (float)(std::sqrt(std::max(0.0, Qw[0][0] + Qw[1][1])) * out.sigmaUere);
            out.vErrM = (float)(std::sqrt(std::max(0.0, Qw[2][2])) * out.sigmaUere);
        }
    }
    return true;
}

} // namespace dect2
