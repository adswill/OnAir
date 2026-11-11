// Lightning position (network): WGS-84 geodesics (Vincenty), the propagation models, the robust TDOA least squares with its error ellipse, and
// the server's matching of detections into strikes (ltpos.h).
#include "dect2/sferics/ltpos.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace dect2::ltpos {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kA = 6378137.0, kF = 1 / 298.257223563, kB = kA * (1 - kF);
constexpr double kE2 = kF * (2 - kF);
constexpr double kRmean = 6371008.8;

double sphereM(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * kDeg, p2 = lat2 * kDeg, dl = (lon2 - lon1) * kDeg;
    const double h = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * kRmean * std::asin(std::min(1.0, std::sqrt(h)));
}
double normLon(double l) { while (l > 180) l -= 360; while (l <= -180) l += 360; return l; }
}

double geodesicM(double lat1, double lon1, double lat2, double lon2, double* azDeg) {
    const double L = normLon(lon2 - lon1) * kDeg;
    const double U1 = std::atan((1 - kF) * std::tan(lat1 * kDeg)), U2 = std::atan((1 - kF) * std::tan(lat2 * kDeg));
    const double sU1 = std::sin(U1), cU1 = std::cos(U1), sU2 = std::sin(U2), cU2 = std::cos(U2);
    double lam = L, lamP, sS = 0, cS = 0, sig = 0, cA2 = 0, c2Sm = 0, sL = 0, cL = 0;
    int it = 0;
    do {
        sL = std::sin(lam); cL = std::cos(lam);
        sS = std::sqrt((cU2 * sL) * (cU2 * sL) + (cU1 * sU2 - sU1 * cU2 * cL) * (cU1 * sU2 - sU1 * cU2 * cL));
        if (sS == 0) { if (azDeg) *azDeg = 0; return 0; }
        cS = sU1 * sU2 + cU1 * cU2 * cL;
        sig = std::atan2(sS, cS);
        const double sA = cU1 * cU2 * sL / sS;
        cA2 = 1 - sA * sA;
        c2Sm = cA2 != 0 ? cS - 2 * sU1 * sU2 / cA2 : 0;
        const double C = kF / 16 * cA2 * (4 + kF * (4 - 3 * cA2));
        lamP = lam;
        lam = L + (1 - C) * kF * sA * (sig + C * sS * (c2Sm + C * cS * (-1 + 2 * c2Sm * c2Sm)));
    } while (std::fabs(lam - lamP) > 1e-12 && ++it < 200);
    if (it >= 200) {   // nearly antipodal: the sphere is close enough there
        if (azDeg) *azDeg = 0;
        return sphereM(lat1, lon1, lat2, lon2);
    }
    const double u2 = cA2 * (kA * kA - kB * kB) / (kB * kB);
    const double A = 1 + u2 / 16384 * (4096 + u2 * (-768 + u2 * (320 - 175 * u2)));
    const double B = u2 / 1024 * (256 + u2 * (-128 + u2 * (74 - 47 * u2)));
    const double dS = B * sS * (c2Sm + B / 4 * (cS * (-1 + 2 * c2Sm * c2Sm) - B / 6 * c2Sm * (-3 + 4 * sS * sS) * (-3 + 4 * c2Sm * c2Sm)));
    if (azDeg) { double a = std::atan2(cU2 * sL, cU1 * sU2 - sU1 * cU2 * cL) / kDeg; *azDeg = a < 0 ? a + 360 : a; }
    return kB * A * (sig - dS);
}

void geodesicDirect(double lat, double lon, double azDeg, double distM, double& lat2, double& lon2) {
    const double a1 = azDeg * kDeg, sa1 = std::sin(a1), ca1 = std::cos(a1);
    const double tU1 = (1 - kF) * std::tan(lat * kDeg), cU1 = 1 / std::sqrt(1 + tU1 * tU1), sU1 = tU1 * cU1;
    const double s1 = std::atan2(tU1, ca1);
    const double sA = cU1 * sa1, cA2 = 1 - sA * sA;
    const double u2 = cA2 * (kA * kA - kB * kB) / (kB * kB);
    const double A = 1 + u2 / 16384 * (4096 + u2 * (-768 + u2 * (320 - 175 * u2)));
    const double B = u2 / 1024 * (256 + u2 * (-128 + u2 * (74 - 47 * u2)));
    double sig = distM / (kB * A), sigP, c2Sm = 0, sS = 0, cS = 0;
    int it = 0;
    do {
        c2Sm = std::cos(2 * s1 + sig); sS = std::sin(sig); cS = std::cos(sig);
        const double dS = B * sS * (c2Sm + B / 4 * (cS * (-1 + 2 * c2Sm * c2Sm) - B / 6 * c2Sm * (-3 + 4 * sS * sS) * (-3 + 4 * c2Sm * c2Sm)));
        sigP = sig;
        sig = distM / (kB * A) + dS;
    } while (std::fabs(sig - sigP) > 1e-12 && ++it < 200);
    const double tmp = sU1 * sS - cU1 * cS * ca1;
    lat2 = std::atan2(sU1 * cS + cU1 * sS * ca1, (1 - kF) * std::sqrt(sA * sA + tmp * tmp)) / kDeg;
    const double lam = std::atan2(sS * sa1, cU1 * cS - sU1 * sS * ca1);
    const double C = kF / 16 * cA2 * (4 + kF * (4 - 3 * cA2));
    const double L = lam - (1 - C) * kF * sA * (sig + C * sS * (c2Sm + C * cS * (-1 + 2 * c2Sm * c2Sm)));
    lon2 = normLon(lon + L / kDeg);
}

double propagationS(const PropModel& pm, double distM) {
    const double vf = pm.velocityFactor > 0.5 ? pm.velocityFactor : 0.9993;
    if (!pm.hops || distM < pm.hopFromKm * 1e3) return distM / (kC * vf);
    const double h = pm.hopHeightKm * 1e3;
    const int n = std::max(1, (int)std::ceil(distM / (std::max(500.0, pm.hopMaxKm) * 1e3)));
    const double half = distM / kRmean / n / 2;
    const double L = std::sqrt(kRmean * kRmean + (kRmean + h) * (kRmean + h) - 2 * kRmean * (kRmean + h) * std::cos(half));
    return 2.0 * n * L / kC;
}

double chi2Quantile(double p, int dof) {
    if (dof <= 0) return 0;
    // the normal quantile (Acklam's rational approximation is overkill here: a few fixed levels are used)
    const double pp = std::clamp(p, 0.5, 0.999999);
    const double t = std::sqrt(-2 * std::log(1 - pp));
    const double z = t - (2.515517 + 0.802853 * t + 0.010328 * t * t) / (1 + 1.432788 * t + 0.189269 * t * t + 0.001308 * t * t * t);
    const double k = dof, a = 2.0 / (9 * k);
    const double c = 1 - a + z * std::sqrt(a);
    return k * c * c * c;
}

// ---------------------------------------------------------------- the solver
namespace {

struct Ctx {
    const std::vector<Obs>* obs;
    const PropModel* pm;
    std::vector<char> use;
    std::vector<char> hop;    // the propagation model per observation, fixed for one descent
    std::vector<int> gi;      // group index 0..G-1
    int G = 0;
};

double travel(const Ctx& c, size_t i, double lat, double lon) {
    const Obs& o = (*c.obs)[i];
    PropModel pm = *c.pm;
    pm.hops = c.hop[i] != 0;
    if (pm.hops) pm.hopFromKm = 0;
    return propagationS(pm, geodesicM(lat, lon, o.lat, o.lon));
}

// the times after folding the ambiguous ones onto their group, the group offsets, and the weighted residuals
struct Eval { std::vector<double> T, t, r; std::vector<double> t0; double chi2 = 0; };
Eval evaluate(const Ctx& c, double lat, double lon, const std::vector<double>* weights) {
    const auto& ob = *c.obs;
    Eval e;
    const size_t n = ob.size();
    e.T.assign(n, 0); e.t.assign(n, 0); e.r.assign(n, std::numeric_limits<double>::quiet_NaN());
    e.t0.assign((size_t)c.G, 0);
    for (size_t i = 0; i < n; i++) if (c.use[i]) { e.T[i] = travel(c, i, lat, lon); e.t[i] = ob[i].tS; }
    for (int g = 0; g < c.G; g++) {
        // the anchor of the group: its best-timed observation that is not ambiguous, else its best-timed one
        int a = -1;
        auto better = [&](size_t i) {
            if (a < 0) return true;
            const Obs& x = ob[i]; const Obs& y = ob[(size_t)a];
            if ((x.ambS > 0) != (y.ambS > 0)) return x.ambS <= 0;
            return x.sigmaS < y.sigmaS;
        };
        for (size_t i = 0; i < n; i++) if (c.use[i] && c.gi[i] == g && better(i)) a = (int)i;
        if (a < 0) continue;
        const double v0 = e.t[(size_t)a] - e.T[(size_t)a];
        double sw = 0, sv = 0;
        for (size_t i = 0; i < n; i++) {
            if (!c.use[i] || c.gi[i] != g) continue;
            if (ob[i].ambS > 0) { const double v = e.t[i] - e.T[i]; e.t[i] -= std::round((v - v0) / ob[i].ambS) * ob[i].ambS; }
            const double w = (weights ? (*weights)[i] : 1.0) / (ob[i].sigmaS * ob[i].sigmaS);
            sw += w; sv += w * (e.t[i] - e.T[i]);
        }
        e.t0[(size_t)g] = sw > 0 ? sv / sw : 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (!c.use[i]) continue;
        e.r[i] = e.t[i] - e.T[i] - e.t0[(size_t)c.gi[i]];
        e.chi2 += (e.r[i] / ob[i].sigmaS) * (e.r[i] / ob[i].sigmaS);
    }
    return e;
}

bool solveLinear(std::vector<double>& A, std::vector<double>& b, int n) {
    for (int c = 0; c < n; c++) {
        int p = c;
        for (int r = c + 1; r < n; r++) if (std::fabs(A[(size_t)(r * n + c)]) > std::fabs(A[(size_t)(p * n + c)])) p = r;
        if (std::fabs(A[(size_t)(p * n + c)]) < 1e-300) return false;
        if (p != c) { for (int k = 0; k < n; k++) std::swap(A[(size_t)(p * n + k)], A[(size_t)(c * n + k)]); std::swap(b[(size_t)p], b[(size_t)c]); }
        for (int r = 0; r < n; r++) {
            if (r == c) continue;
            const double f = A[(size_t)(r * n + c)] / A[(size_t)(c * n + c)];
            if (f == 0) continue;
            for (int k = c; k < n; k++) A[(size_t)(r * n + k)] -= f * A[(size_t)(c * n + k)];
            b[(size_t)r] -= f * b[(size_t)c];
        }
    }
    for (int c = 0; c < n; c++) b[(size_t)c] /= A[(size_t)(c * n + c)];
    return true;
}

bool invert(std::vector<double> A, int n, std::vector<double>& inv) {
    inv.assign((size_t)(n * n), 0);
    for (int col = 0; col < n; col++) {
        std::vector<double> a = A, b((size_t)n, 0);
        b[(size_t)col] = 1;
        if (!solveLinear(a, b, n)) return false;
        for (int r = 0; r < n; r++) inv[(size_t)(r * n + col)] = b[(size_t)r];
    }
    return true;
}

void metres(double lat, double& mN, double& mE) {   // metres per degree north and east
    const double s = std::sin(lat * kDeg), w = std::sqrt(1 - kE2 * s * s);
    mN = kA * (1 - kE2) / (w * w * w) * kDeg;
    mE = kA / w * std::cos(lat * kDeg) * kDeg;
    if (std::fabs(mE) < 1) mE = 1;
}

// Gauss-Newton with Huber weights from (lat, lon); fills the Jacobian-based normal matrix of the last step
struct Descent { double lat = 0, lon = 0, chi2 = 1e300; bool ok = false; std::vector<double> nm; Eval ev; };
Descent descend(Ctx& c, double lat, double lon, const SolveOpts& so) {
    const auto& ob = *c.obs;
    const size_t n = ob.size();
    const int P = 2 + c.G;
    for (size_t i = 0; i < n; i++) c.hop[i] = c.pm->hops && geodesicM(lat, lon, ob[i].lat, ob[i].lon) >= c.pm->hopFromKm * 1e3;
    std::vector<double> hw(n, 1.0);
    Descent d;
    for (int it = 0; it < so.maxIter; it++) {
        Eval e = evaluate(c, lat, lon, &hw);
        double mN, mE;
        metres(lat, mN, mE);
        const double h = 2.0;   // metres
        std::vector<double> A((size_t)(P * P), 0), b((size_t)P, 0);
        for (size_t i = 0; i < n; i++) {
            if (!c.use[i]) continue;
            const double dTN = (travel(c, i, lat + h / mN, lon) - e.T[i]) / h;
            const double dTE = (travel(c, i, lat, lon + h / mE) - e.T[i]) / h;
            std::vector<double> J((size_t)P, 0);
            J[0] = dTN; J[1] = dTE; J[(size_t)(2 + c.gi[i])] = 1;
            const double z = std::fabs(e.r[i] / ob[i].sigmaS);
            hw[i] = z > so.huberK ? so.huberK / z : 1.0;
            const double w = hw[i] / (ob[i].sigmaS * ob[i].sigmaS);
            for (int p = 0; p < P; p++) { b[(size_t)p] += w * J[(size_t)p] * e.r[i]; for (int q = 0; q < P; q++) A[(size_t)(p * P + q)] += w * J[(size_t)p] * J[(size_t)q]; }
        }
        // a little damping keeps a far start from jumping across the globe
        for (int p = 0; p < 2; p++) A[(size_t)(p * P + p)] *= 1.0 + 1e-9;
        std::vector<double> A0 = A;
        if (!solveLinear(A, b, P)) return d;
        double dN = b[0], dE = b[1];
        const double step = std::hypot(dN, dE);
        if (step > 500e3) { dN *= 500e3 / step; dE *= 500e3 / step; }
        lat += dN / mN; lon = normLon(lon + dE / mE);
        lat = std::clamp(lat, -89.9, 89.9);
        if (step < 0.05) break;
    }
    // the final evaluation with plain weights, and the normal matrix for the covariance
    Eval e = evaluate(c, lat, lon, nullptr);
    double mN, mE;
    metres(lat, mN, mE);
    const double h = 2.0;
    std::vector<double> A((size_t)(P * P), 0);
    for (size_t i = 0; i < n; i++) {
        if (!c.use[i]) continue;
        const double dTN = (travel(c, i, lat + h / mN, lon) - e.T[i]) / h;
        const double dTE = (travel(c, i, lat, lon + h / mE) - e.T[i]) / h;
        std::vector<double> J((size_t)P, 0);
        J[0] = dTN; J[1] = dTE; J[(size_t)(2 + c.gi[i])] = 1;
        const double w = 1.0 / (ob[i].sigmaS * ob[i].sigmaS);
        for (int p = 0; p < P; p++) for (int q = 0; q < P; q++) A[(size_t)(p * P + q)] += w * J[(size_t)p] * J[(size_t)q];
    }
    d.lat = lat; d.lon = lon; d.chi2 = e.chi2; d.ok = std::isfinite(e.chi2); d.nm = A; d.ev = e;
    return d;
}

void centroid(const std::vector<Obs>& ob, const std::vector<char>& use, double& lat, double& lon) {
    double x = 0, y = 0, z = 0;
    for (size_t i = 0; i < ob.size(); i++) {
        if (!use[i]) continue;
        const double p = ob[i].lat * kDeg, l = ob[i].lon * kDeg;
        x += std::cos(p) * std::cos(l); y += std::cos(p) * std::sin(l); z += std::sin(p);
    }
    lat = std::atan2(z, std::hypot(x, y)) / kDeg;
    lon = std::atan2(y, x) / kDeg;
}

int dofOf(const Ctx& c) {
    int n = 0;
    std::set<int> g;
    for (size_t i = 0; i < c.use.size(); i++) if (c.use[i]) { n++; g.insert(c.gi[i]); }
    return n - 2 - (int)g.size();
}

} // namespace

Fix solveTdoa(const std::vector<Obs>& obs, const PropModel& pm, const SolveOpts& so) {
    Fix f;
    const size_t n = obs.size();
    Ctx c;
    c.obs = &obs; c.pm = &pm;
    c.use.assign(n, 1);
    c.hop.assign(n, 0);
    std::map<int, int> gmap;
    for (const Obs& o : obs) if (!gmap.count(o.group)) { const int k = (int)gmap.size(); gmap[o.group] = k; }
    c.G = (int)gmap.size();
    for (const Obs& o : obs) c.gi.push_back(gmap[o.group]);
    for (size_t i = 0; i < n; i++) if (!(obs[i].sigmaS > 0) || !std::isfinite(obs[i].tS)) c.use[i] = 0;
    // every group needs two observations to say anything; a lone one only carries its own offset
    for (int g = 0; g < c.G; g++) {
        int k = 0;
        for (size_t i = 0; i < n; i++) if (c.use[i] && c.gi[i] == g) k++;
        if (k < 2) for (size_t i = 0; i < n; i++) if (c.gi[i] == g) c.use[i] = 0;
    }
    if (dofOf(c) < (so.allowThree ? 0 : 1)) { f.why = "too few stations"; return f; }

    // start grid around the stations' centre, the cost minimised over the group offsets at each point
    double cl, co;
    centroid(obs, c.use, cl, co);
    for (size_t i = 0; i < n; i++) c.hop[i] = 0;
    struct Cand { double cost, lat, lon; };
    std::vector<Cand> cands;
    static const double kRadiiKm[] = {0, 30, 80, 150, 250, 400, 600, 900, 1300, 1800, 2500, 3200, 4000, 5000, 6500, 8000};
    for (double rk : kRadiiKm) {
        if (rk > so.searchKm) break;
        const int nb = rk == 0 ? 1 : 24;
        for (int k = 0; k < nb; k++) {
            double la = cl, lo = co;
            if (rk > 0) geodesicDirect(cl, co, k * 15.0 + (rk > 1000 ? 7.5 : 0), rk * 1e3, la, lo);
            for (size_t i = 0; i < n; i++) c.hop[i] = pm.hops && geodesicM(la, lo, obs[i].lat, obs[i].lon) >= pm.hopFromKm * 1e3;
            const Eval e = evaluate(c, la, lo, nullptr);
            cands.push_back({e.chi2, la, lo});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });
    std::vector<Cand> starts;
    for (const Cand& k : cands) {
        bool apart = true;
        for (const Cand& s : starts) if (geodesicM(k.lat, k.lon, s.lat, s.lon) < 150e3) { apart = false; break; }
        if (apart) starts.push_back(k);
        if (starts.size() >= 5) break;
    }

    auto runAll = [&](std::vector<Descent>& all) {
        all.clear();
        for (const Cand& s : starts) { Descent d = descend(c, s.lat, s.lon, so); if (d.ok) all.push_back(d); }
        std::sort(all.begin(), all.end(), [](const Descent& a, const Descent& b) { return a.chi2 < b.chi2; });
    };
    std::vector<Descent> all;
    runAll(all);
    if (all.empty()) { f.why = "no convergence"; return f; }
    // outliers: with two degrees of freedom to spare, the worst residual beyond outlierSigma goes and the fit is done again
    for (;;) {
        const Descent& b = all.front();
        if (dofOf(c) < 2) break;
        size_t worst = n;
        double wz = 0;
        for (size_t i = 0; i < n; i++) if (c.use[i] && std::isfinite(b.ev.r[i])) { const double z = std::fabs(b.ev.r[i] / obs[i].sigmaS); if (z > wz) { wz = z; worst = i; } }
        if (worst == n || wz <= so.outlierSigma) break;
        c.use[worst] = 0;
        f.dropped.push_back(obs[worst].station);
        starts.insert(starts.begin(), Cand{0, b.lat, b.lon});
        runAll(all);
        if (all.empty()) { f.why = "no convergence"; return f; }
    }
    const Descent& best = all.front();
    const int dof = dofOf(c);
    f.dof = dof;
    f.chi2 = best.chi2;
    f.lat = best.lat; f.lon = best.lon;
    f.residualS = best.ev.r;
    f.used = 0;
    double ss = 0;
    for (size_t i = 0; i < n; i++) if (c.use[i]) { f.used++; ss += best.ev.r[i] * best.ev.r[i]; }
    f.rmsS = f.used ? std::sqrt(ss / f.used) : 0;
    const int g0 = gmap.count(0) ? gmap[0] : 0;
    f.t0S = best.ev.t0.empty() ? 0 : best.ev.t0[(size_t)g0];
    // the error ellipse from the covariance of (north, east)
    const int P = 2 + c.G;
    std::vector<double> nm = best.nm;
    for (int g = 0; g < c.G; g++) {   // a group with no observation left: pin its offset so the matrix stays regular
        bool any = false;
        for (size_t i = 0; i < n; i++) if (c.use[i] && c.gi[i] == g) any = true;
        if (!any) nm[(size_t)((2 + g) * P + 2 + g)] = 1;
    }
    std::vector<double> cov;
    if (!invert(nm, P, cov)) { f.why = "singular geometry"; return f; }
    const double scale = dof > 0 ? std::max(1.0, best.chi2 / dof) : 1.0;
    const double cNN = cov[0] * scale, cEE = cov[(size_t)(P + 1)] * scale, cNE = cov[1] * scale;
    const double tr = cNN + cEE, det = cNN * cEE - cNE * cNE;
    const double disc = std::sqrt(std::max(0.0, tr * tr / 4 - det));
    const double l1 = tr / 2 + disc, l2 = std::max(0.0, tr / 2 - disc);
    const double k95 = 2.4477;   // sqrt(chi2(0.95, 2))
    f.semiMajorM = k95 * std::sqrt(std::max(0.0, l1));
    f.semiMinorM = k95 * std::sqrt(l2);
    double az = 0.5 * std::atan2(2 * cNE, cNN - cEE) / kDeg;
    if (az < 0) az += 180;
    f.majorAzDeg = az;
    f.sigmaNorthM = std::sqrt(std::max(0.0, cNN));
    f.sigmaEastM = std::sqrt(std::max(0.0, cEE));
    if (dof >= 1 && best.chi2 > chi2Quantile(so.chi2Prob, dof)) { f.why = "the times do not fit one position (chi-square)"; return f; }
    if (dof == 0) {
        f.unchecked = true;
        for (size_t k = 1; k < all.size(); k++)
            if (all[k].chi2 < 1.0 && geodesicM(all[k].lat, all[k].lon, best.lat, best.lon) > std::max(5e3, 3 * f.semiMajorM)) { f.ambiguous = true; break; }
    }
    if (!std::isfinite(f.semiMajorM)) { f.why = "singular geometry"; return f; }
    f.ok = true;
    return f;
}

// ---------------------------------------------------------------- matching
void Matcher::add(const Det& d) {
    auto it = std::upper_bound(pend_.begin(), pend_.end(), d.tNs, [](int64_t t, const Det& x) { return t < x.tNs; });
    pend_.insert(it, d);
    while (pend_.size() > o_.maxPending) { pend_.pop_front(); st_.unmatched++; }
}

std::vector<Strike> Matcher::process(int64_t nowNs) {
    std::vector<Strike> out;
    const int64_t hold = (int64_t)(o_.holdS * 1e9);
    const double vmin = kC * 0.98;
    while (!pend_.empty() && pend_.front().tNs < nowNs - hold) {
        const Det seed = pend_.front();
        // candidates: per other station, detections within the light time of the baseline plus the timing errors
        std::map<uint32_t, std::vector<size_t>> cand;
        for (size_t j = 1; j < pend_.size(); j++) {
            const Det& d = pend_[j];
            const double base = geodesicM(seed.lat, seed.lon, d.lat, d.lon) / vmin * 1e9;
            const double win = base + o_.slackNs + 5 * std::hypot(seed.sigmaNs, d.sigmaNs) + 3 * std::max(seed.ambNs, d.ambNs);   // a common-view reading may be off by whole symbol periods
            if ((double)(d.tNs - seed.tNs) > 0.1e9 + 10 * seed.sigmaNs) break;   // sorted: nothing later can fit (100 ms is more than any baseline)
            if (d.station == seed.station) continue;
            if (std::fabs((double)(d.tNs - seed.tNs)) > win) continue;
            if (seed.widthUs > 0 && d.widthUs > 0 && std::max(seed.widthUs, d.widthUs) / std::min(seed.widthUs, d.widthUs) > o_.maxWidthRatio) continue;
            cand[d.station].push_back(j);
        }
        for (auto& kv : cand)
            std::sort(kv.second.begin(), kv.second.end(), [&](size_t a, size_t b) { return std::llabs(pend_[a].tNs - seed.tNs) < std::llabs(pend_[b].tNs - seed.tNs); });
        std::map<uint32_t, size_t> pick;   // station -> index into its candidate list
        for (const auto& kv : cand) pick[kv.first] = 0;
        st_.groups++;
        bool solved = false;
        Strike sk;
        for (int attempt = 0; attempt < 8 && !solved; attempt++) {
            // the group: the seed and each station's current pick; pairwise light-time check among the fine ones
            std::vector<size_t> idx{0};
            for (const auto& kv : pick) idx.push_back(cand[kv.first][kv.second]);
            std::vector<size_t> fine;
            for (size_t k : idx) if (pend_[k].fine) fine.push_back(k);
            for (bool again = true; again && fine.size() > 2;) {
                again = false;
                std::vector<int> bad(fine.size(), 0);
                for (size_t a = 0; a < fine.size(); a++)
                    for (size_t b = a + 1; b < fine.size(); b++) {
                        const Det& x = pend_[fine[a]]; const Det& y = pend_[fine[b]];
                        if (x.group != y.group) continue;
                        const double lim = geodesicM(x.lat, x.lon, y.lat, y.lon) / vmin * 1e9 + o_.slackNs + 5 * std::hypot(x.sigmaNs, y.sigmaNs) + 3 * std::max(x.ambNs, y.ambNs);
                        if (std::fabs((double)(x.tNs - y.tNs)) > lim) { bad[a]++; bad[b]++; }
                    }
                size_t w = 0;
                for (size_t a = 1; a < fine.size(); a++) if (bad[a] > bad[w] || (bad[a] == bad[w] && fine[a] != 0 && fine[w] == 0)) w = a;
                if (bad[w] > 0 && fine[w] != 0) { fine.erase(fine.begin() + (long)w); again = true; }
            }
            if ((int)fine.size() < std::max(3, std::min(o_.minStations, 64))) {
                if (attempt == 0) { if (idx.size() >= 3) st_.coarseOnly++; else st_.tooFew++; }
                break;
            }
            std::vector<Obs> ob;
            for (size_t k : fine) {
                const Det& d = pend_[k];
                Obs o;
                o.station = d.station; o.lat = d.lat; o.lon = d.lon;
                o.tS = (double)(d.tNs - seed.tNs) * 1e-9;
                o.sigmaS = std::max(1.0, d.sigmaNs) * 1e-9;
                o.group = d.group;
                o.ambS = d.ambNs * 1e-9;
                ob.push_back(o);
            }
            SolveOpts so = so_;
            so.allowThree = o_.minStations <= 3;
            Fix fx = solveTdoa(ob, pm_, so);
            if (fx.ok) {
                solved = true;
                sk.fix = fx;
                sk.timeNs = seed.tNs + (int64_t)std::llround(fx.t0S * 1e9);
                std::set<uint32_t> dropped(fx.dropped.begin(), fx.dropped.end());
                std::vector<size_t> used;
                bool g0 = false;
                for (size_t q = 0; q < fine.size(); q++) {
                    if (dropped.count(pend_[fine[q]].station)) continue;
                    sk.stations.push_back(pend_[fine[q]].station);
                    used.push_back(fine[q]);
                    if (pend_[fine[q]].group == 0) g0 = true;
                }
                sk.utc = g0;
                // coarse detections that fit the strike within their own error: heard there too
                for (size_t k : idx) {
                    const Det& d = pend_[k];
                    if (d.fine) continue;
                    const double tt = propagationS(pm_, geodesicM(fx.lat, fx.lon, d.lat, d.lon)) * 1e9;
                    if (std::fabs((double)(d.tNs - sk.timeNs) - tt) <= 5 * d.sigmaNs + o_.slackNs) { sk.alsoHeard.push_back(d.station); used.push_back(k); }
                }
                std::sort(used.begin(), used.end());
                for (size_t q = used.size(); q-- > 0;) pend_.erase(pend_.begin() + (long)used[q]);
                if (std::find(used.begin(), used.end(), (size_t)0) == used.end()) pend_.pop_front();   // the seed itself was an outlier
                out.push_back(sk);
                st_.fixes++;
                break;
            }
            // not one position: move the station with the worst residual to its next candidate
            uint32_t worstSt = 0;
            double wz = -1;
            for (size_t q = 0; q < ob.size(); q++) {
                if (fx.residualS.size() != ob.size() || !std::isfinite(fx.residualS[q])) continue;
                const double z = std::fabs(fx.residualS[q] / ob[q].sigmaS);
                if (z > wz && pick.count(ob[q].station) && pick[ob[q].station] + 1 < cand[ob[q].station].size()) { wz = z; worstSt = ob[q].station; }
            }
            if (wz < 0) { st_.rejected++; break; }
            pick[worstSt]++;
            if (attempt == 7) st_.rejected++;
        }
        if (!solved) { pend_.pop_front(); st_.unmatched++; }
    }
    return out;
}

} // namespace dect2::ltpos
