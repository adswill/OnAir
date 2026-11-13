// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Antenna designer: the optimiser. Nelder-Mead (downhill simplex) in the box of the chosen template parameters, scaled to 0..1; the cost
// is the worst SWR over the band above its target, the gain below its target and the front to back below its target, weighted.
#include "dect2/antd/antd_tools.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace dect2::antd {

std::vector<double> nelderMead(const std::function<double(const std::vector<double>&)>& f, std::vector<double> x0,
                               const std::vector<double>& lo, const std::vector<double>& hi, int maxEvals, double tol, int* evalsOut,
                               const std::function<bool(int, double, const std::vector<double>&)>& progress) {
    const int n = (int)x0.size();
    int evals = 0;
    if (n == 0) { if (evalsOut) *evalsOut = 0; return x0; }
    auto clampv = [&](std::vector<double> x) { for (int i = 0; i < n; ++i) x[i] = std::min(hi[i], std::max(lo[i], x[i])); return x; };
    auto fe = [&](const std::vector<double>& x) { ++evals; return f(x); };
    std::vector<std::vector<double>> s(n + 1, clampv(x0));
    for (int i = 0; i < n; ++i) {
        const double step = 0.1 * (hi[i] - lo[i]);
        s[i + 1][i] += (s[i + 1][i] + step <= hi[i]) ? step : -step;
        s[i + 1] = clampv(s[i + 1]);
    }
    std::vector<double> fv(n + 1);
    for (int i = 0; i <= n; ++i) fv[i] = fe(s[i]);
    double lastBest = 1e300;
    bool stop = false;
    while (evals < maxEvals && !stop) {
        std::vector<int> ix(n + 1);
        std::iota(ix.begin(), ix.end(), 0);
        std::sort(ix.begin(), ix.end(), [&](int a, int b) { return fv[a] < fv[b]; });
        std::vector<std::vector<double>> s2(n + 1); std::vector<double> f2(n + 1);
        for (int i = 0; i <= n; ++i) { s2[i] = s[ix[i]]; f2[i] = fv[ix[i]]; }
        s = s2; fv = f2;
        if (fv[0] < lastBest) { lastBest = fv[0]; if (progress && !progress(evals, fv[0], s[0])) break; }
        // converged: the simplex is small and the values agree
        double size = 0;
        for (int i = 1; i <= n; ++i) for (int j = 0; j < n; ++j) size = std::max(size, std::fabs(s[i][j] - s[0][j]) / std::max(hi[j] - lo[j], 1e-30));
        if (size < tol && std::fabs(fv[n] - fv[0]) < tol * (1 + std::fabs(fv[0]))) break;
        std::vector<double> c(n, 0);
        for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) c[j] += s[i][j] / n;
        auto along = [&](double t) { std::vector<double> x(n); for (int j = 0; j < n; ++j) x[j] = c[j] + t * (s[n][j] - c[j]); return clampv(x); };
        const std::vector<double> xr = along(-1); const double fr = fe(xr);
        if (fr < fv[0]) {
            const std::vector<double> xe = along(-2); const double fe2 = fe(xe);
            if (fe2 < fr) { s[n] = xe; fv[n] = fe2; } else { s[n] = xr; fv[n] = fr; }
        } else if (fr < fv[n - 1]) {
            s[n] = xr; fv[n] = fr;
        } else {
            const bool outside = fr < fv[n];
            const std::vector<double> xc = along(outside ? -0.5 : 0.5); const double fc = fe(xc);
            if (fc < (outside ? fr : fv[n])) { s[n] = xc; fv[n] = fc; }
            else {   // shrink toward the best
                for (int i = 1; i <= n; ++i) {
                    for (int j = 0; j < n; ++j) s[i][j] = s[0][j] + 0.5 * (s[i][j] - s[0][j]);
                    fv[i] = fe(s[i]);
                    if (evals >= maxEvals) break;
                }
            }
        }
        if (evals >= maxEvals) stop = true;
    }
    int best = 0;
    for (int i = 1; i <= n; ++i) if (fv[i] < fv[best]) best = i;
    if (evalsOut) *evalsOut = evals;
    return s[best];
}

namespace {
struct Eval { double cost = 1e9, swr = 1e6, gain = -999, fb = 0; bool ok = false; };

Eval evaluate(const std::string& id, double fd, std::vector<TParam> p, const std::vector<OptVar>& vars, const std::vector<double>& x,
              const OptGoal& g, const Ground& ground) {
    Eval e;
    for (size_t i = 0; i < vars.size(); ++i)
        for (auto& q : p) if (q.key == vars[i].key) q.value = (q.kind == PKind::Count) ? std::round(x[i]) : x[i];
    Model m = buildTemplate(id, fd, p, nullptr, &ground);
    const double f0 = g.f0Hz > 0 ? g.f0Hz : fd, f1 = g.f1Hz > 0 ? g.f1Hz : fd;
    const int np = std::max(1, f1 > f0 ? g.points : 1);
    const Mesh mesh = buildMesh(m, std::max(f0, f1));
    double worst = 1;
    Solution centre;
    for (int i = 0; i < np; ++i) {
        const double f = np == 1 ? 0.5 * (f0 + f1) : f0 + (f1 - f0) * i / (np - 1);
        Solution s = solveMesh(m, mesh, f, 1);
        if (!s.ok) return e;
        worst = std::max(worst, std::min(vswr(s.zin, g.z0), 50.0));
        if (i == np / 2) centre = std::move(s);
    }
    e.ok = true; e.swr = worst;
    double cost = g.swrWeight * (std::max(0.0, worst - g.swrTarget) + 0.02 * (worst - 1));
    if (g.gainWeight > 0 || g.fbWeight > 0) {
        const double th = 90 - g.elevationDeg;
        e.gain = gainAt(m, centre, th, 0).total;
        e.fb = e.gain - gainAt(m, centre, th, 180).total;
        cost += g.gainWeight * std::max(0.0, g.gainTarget - e.gain) - 0.01 * g.gainWeight * e.gain;
        cost += g.fbWeight * std::max(0.0, g.fbTarget - std::min(e.fb, 60.0));
    }
    e.cost = cost;
    return e;
}
} // namespace

bool trimParam(const std::string& id, double f, std::vector<TParam>& prm, const std::string& key, const Ground* ground, double z0, cd* zOut) {
    TParam* p = nullptr;
    for (auto& q : prm) if (q.key == key) p = &q;
    if (!p) return false;
    auto zAt = [&](double x, bool& ok) {
        const double keep = p->value;
        p->value = x;
        const Model m = buildTemplate(id, f, prm, nullptr, ground);
        p->value = keep;
        const Solution s = solve(m, f);
        ok = s.ok;
        return s.zin;
    };
    // look for a sign change of X on a coarse grid near the current value first, then the whole range
    const double lo = p->lo, hi = p->hi;
    const int n = 24;
    std::vector<double> xs(n + 1); std::vector<cd> zs(n + 1); std::vector<bool> oks(n + 1);
    for (int i = 0; i <= n; ++i) { bool ok; xs[i] = lo + (hi - lo) * i / n; zs[i] = zAt(xs[i], ok); oks[i] = ok; }
    int bracket = -1; double bestD = 1e300;
    for (int i = 0; i < n; ++i) {
        if (!oks[i] || !oks[i + 1]) continue;
        if ((zs[i].imag() <= 0) != (zs[i + 1].imag() <= 0) && std::abs(zs[i]) < 1e5 && std::abs(zs[i + 1]) < 1e5) {
            // the crossing with the resistance nearest z0 (a resonance, not an anti-resonance, for a series-fed antenna)
            const double d = std::fabs(0.5 * (zs[i].real() + zs[i + 1].real()) - z0) + std::fabs(0.5 * (xs[i] + xs[i + 1]) - p->value) / (hi - lo) * z0;
            if (d < bestD) { bestD = d; bracket = i; }
        }
    }
    double x;
    if (bracket >= 0) {
        double a = xs[bracket], b = xs[bracket + 1];
        const bool aNeg = zs[bracket].imag() <= 0;
        for (int it = 0; it < 40; ++it) {
            const double mid = 0.5 * (a + b);
            bool ok; const cd z = zAt(mid, ok);
            if (!ok) break;
            if ((z.imag() <= 0) == aNeg) a = mid; else b = mid;
            if (b - a < 1e-7 * (hi - lo)) break;
        }
        x = 0.5 * (a + b);
    } else {   // no zero crossing: the lowest SWR (golden section around the best grid point)
        int bi = 0;
        for (int i = 1; i <= n; ++i) if (oks[i] && (!oks[bi] || vswr(zs[i], z0) < vswr(zs[bi], z0))) bi = i;
        double a = xs[std::max(0, bi - 1)], b = xs[std::min(n, bi + 1)];
        const double g = 0.6180339887;
        double c = b - g * (b - a), d = a + g * (b - a);
        bool ok;
        double fc = vswr(zAt(c, ok), z0), fd = vswr(zAt(d, ok), z0);
        for (int it = 0; it < 40; ++it) {
            if (fc < fd) { b = d; d = c; fd = fc; c = b - g * (b - a); fc = vswr(zAt(c, ok), z0); }
            else { a = c; c = d; fc = fd; d = a + g * (b - a); fd = vswr(zAt(d, ok), z0); }
        }
        x = 0.5 * (a + b);
    }
    p->value = p->kind == PKind::Count ? std::round(x) : x;
    if (zOut) { bool ok; *zOut = zAt(p->value, ok); }
    return true;
}

OptResult optimise(const std::string& id, double fd, const std::vector<TParam>& start, const std::vector<OptVar>& vars, const OptGoal& g,
                   const Ground& ground, int maxEvals, const std::function<bool(const OptStep&)>& progress) {
    OptResult r;
    if (vars.empty()) { r.msg = "choose at least one parameter to vary"; return r; }
    std::vector<double> x0, lo, hi;
    for (const auto& v : vars) {
        double val = paramValue(start, v.key, 0.5 * (v.lo + v.hi));
        lo.push_back(std::min(v.lo, v.hi)); hi.push_back(std::max(v.lo, v.hi));
        x0.push_back(std::min(hi.back(), std::max(lo.back(), val)));
    }
    auto cost = [&](const std::vector<double>& x) { return evaluate(id, fd, start, vars, x, g, ground).cost; };
    int iter = 0;
    auto prog = [&](int ev, double c, const std::vector<double>& x) {
        if (!progress) return true;
        OptStep st; st.iter = ++iter; st.cost = c; st.x = x;
        const Eval e = evaluate(id, fd, start, vars, x, g, ground);
        st.swr = e.swr; st.gain = e.gain; st.fb = e.fb;
        (void)ev;
        return progress(st);
    };
    const std::vector<double> best = nelderMead(cost, x0, lo, hi, maxEvals, 1e-4, &r.evals, prog);
    const Eval e = evaluate(id, fd, start, vars, best, g, ground);
    r.ok = e.ok;
    r.params = start;
    for (size_t i = 0; i < vars.size(); ++i)
        for (auto& q : r.params) if (q.key == vars[i].key) q.value = (q.kind == PKind::Count) ? std::round(best[i]) : best[i];
    r.cost = e.cost; r.worstSwr = e.swr; r.gainDbi = e.gain; r.fbDb = e.fb;
    r.msg = e.ok ? "done" : "the best point did not solve";
    return r;
}

} // namespace dect2::antd
