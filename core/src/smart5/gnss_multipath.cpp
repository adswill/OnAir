// Multipath indicator (see gnss_multipath.h).
#include "dect2/smart5/gnss_multipath.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2 {
namespace smart5 {

namespace { const double kPi = 3.14159265358979323846; }

void GnssMultipath::sample(int prn, double el, double az, double cn0) {
    if (el < 5 || el > 89 || !std::isfinite(cn0)) return;
    s_[prn].push_back({el, az, cn0});
}

MultipathResult GnssMultipath::analyse(int prn) const {
    MultipathResult r;
    auto it = s_.find(prn);
    if (it == s_.end() || it->second.size() < 60) return r;
    // bin C/N0 into 200 bins of sin(elevation)
    double lo = 1, hi = 0;
    for (auto& p : it->second) { const double x = std::sin(p.el * kPi / 180); lo = std::min(lo, x); hi = std::max(hi, x); }
    if (hi - lo < 0.15) return r;   // too little elevation change to see a fringe
    const int nb = 200;
    std::vector<double> sum(nb, 0), cnt(nb, 0);
    double az = 0;
    for (auto& p : it->second) {
        const double x = std::sin(p.el * kPi / 180);
        const int b = std::min(nb - 1, (int)((x - lo) / (hi - lo) * nb));
        sum[b] += p.cn0; cnt[b] += 1; az += p.az;
    }
    r.azDeg = az / it->second.size();
    std::vector<double> xs, ys;
    for (int b = 0; b < nb; b++) if (cnt[b] > 0) { xs.push_back(lo + (b + 0.5) / nb * (hi - lo)); ys.push_back(sum[b] / cnt[b]); }
    if (xs.size() < 40) return r;
    // the smooth trend: a cubic in sin(elevation) by least squares
    double A[4][5] = {{0}};
    for (size_t i = 0; i < xs.size(); i++) {
        double v[4] = {1, xs[i], xs[i] * xs[i], xs[i] * xs[i] * xs[i]};
        for (int a = 0; a < 4; a++) { for (int b = 0; b < 4; b++) A[a][b] += v[a] * v[b]; A[a][4] += v[a] * ys[i]; }
    }
    for (int i = 0; i < 4; i++) {
        int pv = i; for (int k = i + 1; k < 4; k++) if (std::fabs(A[k][i]) > std::fabs(A[pv][i])) pv = k;
        for (int k = 0; k < 5; k++) std::swap(A[i][k], A[pv][k]);
        if (std::fabs(A[i][i]) < 1e-12) return r;
        for (int k = i + 1; k < 4; k++) { const double m = A[k][i] / A[i][i]; for (int c = i; c < 5; c++) A[k][c] -= m * A[i][c]; }
    }
    double c[4];
    for (int i = 3; i >= 0; i--) { double t = A[i][4]; for (int k = i + 1; k < 4; k++) t -= A[i][k] * c[k]; c[i] = t / A[i][i]; }
    std::vector<double> res(xs.size());
    for (size_t i = 0; i < xs.size(); i++) res[i] = ys[i] - (c[0] + c[1] * xs[i] + c[2] * xs[i] * xs[i] + c[3] * xs[i] * xs[i] * xs[i]);
    // strongest sinusoid of the residual: scan fringe frequencies from 3 cycles over the span up to 90 cycles per unit sin
    const double span = xs.back() - xs.front();
    double bestF = 0, bestA = 0;
    for (double f = 3 / span; f <= 90; f += 0.05 / span) {
        std::complex<double> acc(0, 0);
        for (size_t i = 0; i < xs.size(); i++) acc += res[i] * std::polar(1.0, -2 * kPi * f * xs[i]);
        const double a = 2 * std::abs(acc) / xs.size();
        if (a > bestA) { bestA = a; bestF = f; }
    }
    double rms = 0;
    for (double e : res) rms += e * e;
    rms = std::sqrt(rms / res.size());
    r.valid = true;
    r.depthDb = 2 * bestA;
    r.cyclesPerSin = bestF;
    r.heightM = bestF * lambda_ / 2;
    // a fringe: deep enough, and the sinusoid carries most of the residual (not just noise)
    r.multipath = r.depthDb >= 1.0 && bestA / std::sqrt(2.0) >= 0.6 * rms;
    return r;
}

} // namespace smart5
} // namespace dect2
