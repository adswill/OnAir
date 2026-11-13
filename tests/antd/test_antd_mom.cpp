// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Antenna designer solver against the analytic cases: the half-wave dipole (about 73 ohm at resonance, 2.15 dBi), the quarter-wave monopole
// over a perfect ground (half the dipole, 5.16 dBi), the short dipole's radiation resistance, the one-wavelength loop, a 2-element Yagi,
// a lossless pattern integrating to the input power, the LU against a direct product, sweeps and segment refinement.
#include "dect2/antd/antd.h"
#include "dect2/antd/antd_tools.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2::antd;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (std::fabs(_a - _b) > (tol)) { printf("FAIL line %d: %s = %.4f, want %.4f +- %g\n", __LINE__, #a, _a, _b, (double)(tol)); fails++; } } while (0)

static Model dipole(double lenLam, double radLam, double f) {
    const double lam = kC0 / f;
    Model m; Wire w; w.a = {0, 0, -lenLam * lam / 2}; w.b = {0, 0, lenLam * lam / 2}; w.radius = radLam * lam;
    m.wires.push_back(w); m.sources.push_back({0, 0.5, 1.0});
    m.segPerWavelength = 40;
    return m;
}

int main() {
    const double f = 100e6, lam = kC0 / f;
    // LU: A x = b against the product
    {
        std::mt19937 rng(7); std::uniform_real_distribution<double> u(-1, 1);
        for (int n : {5, 47, 130, 300}) {
            std::vector<cd> a((size_t)n * n), x(n), b(n, 0.0);
            for (auto& v : a) v = cd(u(rng), u(rng));
            for (int i = 0; i < n; ++i) a[(size_t)i * n + i] += 2.0;
            for (auto& v : x) v = cd(u(rng), u(rng));
            for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) b[i] += a[(size_t)i * n + j] * x[j];
            std::vector<int> piv;
            std::vector<cd> lu = a;
            CHECK(luFactor(lu, n, piv, 4));
            luSolve(lu, n, piv, b);
            double e = 0; for (int i = 0; i < n; ++i) e = std::max(e, std::abs(b[i] - x[i]));
            CHECK(e < 1e-9);
        }
    }
    // half-wave dipole, free space: exactly half a wavelength about 73 + j42 (thin wire), resonance a little shorter near 70-73 ohm
    {
        Model m = dipole(0.5, 1e-5, f);
        Solution s = solve(m, f);
        CHECK(s.ok);
        NEAR(s.zin.real(), 76, 4.5);
        NEAR(s.zin.imag(), 42.5, 4);
        Pattern p = pattern(m, s, 5, 10);
        NEAR(p.maxDbi, 2.15, 0.05);
        NEAR(p.maxTheta, 90, 0.1);
        NEAR(p.efficiency, 1.0, 0.01);       // lossless: the far field holds the input power
        Figures fg = figures(m, s, p);
        NEAR(fg.maxDbd, 0, 0.05);
        NEAR(fg.bwElDeg, 78, 3);             // the half-wave dipole's 78 degree E-plane beamwidth
        CHECK(fg.bwAzDeg >= 359);            // omnidirectional in the H plane (the z axis is the wire)
        // resonance: X = 0 between 0.47 and 0.49 wavelength, R there about 70
        double lo = 0.45, hi = 0.5;
        for (int it = 0; it < 30; ++it) {
            const double mid = 0.5 * (lo + hi);
            Solution t = solve(dipole(mid, 1e-5, f), f);
            (t.zin.imag() > 0 ? hi : lo) = mid;
        }
        const double res = 0.5 * (lo + hi);
        CHECK(res > 0.47 && res < 0.495);
        Solution t = solve(dipole(res, 1e-5, f), f);
        NEAR(t.zin.real(), 72, 3);
        printf("dipole: 0.5 lambda Z = %.2f %+.2fj, %.3f dBi; resonant at %.4f lambda, R = %.2f\n", s.zin.real(), s.zin.imag(), p.maxDbi, res, t.zin.real());
    }
    // short dipole: R = 20 pi^2 (L / lambda)^2 for the triangular current
    {
        Model m = dipole(0.05, 1e-6, f);
        m.minSegments = 11;
        Solution s = solve(m, f);
        NEAR(s.zin.real(), 20 * kPi * kPi * 0.0025, 0.03);
        Pattern p = pattern(m, s, 5, 10);
        NEAR(p.maxDbi, 1.76, 0.05);   // 1.5 directivity
    }
    // quarter-wave monopole over a perfect ground: half the dipole impedance, 3 dB more gain (5.16 dBi), maximum on the horizon
    {
        Model g; Wire v; v.a = {0, 0, 0}; v.b = {0, 0, lam / 4}; v.radius = 1e-5 * lam; g.wires.push_back(v);
        g.sources.push_back({0, 0.0, 1.0}); g.ground.type = GroundType::Perfect; g.segPerWavelength = 40;
        Solution s = solve(g, f);
        CHECK(s.ok);
        Solution d = solve(dipole(0.5, 1e-5, f), f);
        NEAR(s.zin.real(), d.zin.real() / 2, 0.5);
        NEAR(s.zin.imag(), d.zin.imag() / 2, 0.5);
        NEAR(s.zin.real(), 37, 3);
        Pattern p = pattern(g, s, 2, 10);
        NEAR(p.maxDbi, 5.16, 0.06);
        NEAR(90 - p.maxTheta, 0, 0.1);
        NEAR(p.efficiency, 1.0, 0.02);
        printf("monopole: Z = %.2f %+.2fj, %.3f dBi\n", s.zin.real(), s.zin.imag(), p.maxDbi);
        // a horizontal dipole a quarter-wave over perfect ground: the maximum straight up, gain about 7.5 dBi (2 x the field)
        Model h = dipole(0.48, 1e-4, f);
        h.wires[0].a = {0, -0.24 * lam, lam / 4}; h.wires[0].b = {0, 0.24 * lam, lam / 4};
        h.ground.type = GroundType::Perfect;
        Solution hs = solve(h, f);
        Pattern hp = pattern(h, hs, 5, 10);
        NEAR(hp.maxTheta, 0, 0.1);
        CHECK(hp.maxDbi > 6.5 && hp.maxDbi < 9.0);
        // a real ground absorbs some: less gain, an efficiency below 1, no maximum on the horizon for a horizontal wire
        h.ground.type = GroundType::Real;
        Solution rs = solve(h, f);
        Pattern rp = pattern(h, rs, 5, 10);
        CHECK(rp.maxDbi < hp.maxDbi);
        CHECK(rp.efficiency < 0.98 && rp.efficiency > 0.3);
    }
    // a one-wavelength square loop: resonates (X = 0) with the perimeter a little above one wavelength, R about 100-140 ohm
    {
        auto loop = [&](double per) {
            Model m; const double s = per * lam / 4;
            const Vec3 c[4] = {{0, -s / 2, -s / 2}, {0, s / 2, -s / 2}, {0, s / 2, s / 2}, {0, -s / 2, s / 2}};
            for (int i = 0; i < 4; ++i) { Wire w; w.a = c[i]; w.b = c[(i + 1) % 4]; w.radius = 1e-4 * lam; m.wires.push_back(w); }
            m.sources.push_back({0, 0.5, 1.0}); m.segPerWavelength = 40;
            return m;
        };
        double lo = 0.95, hi = 1.15;
        for (int it = 0; it < 25; ++it) {
            const double mid = 0.5 * (lo + hi);
            Solution t = solve(loop(mid), f);
            (t.zin.imag() > 0 ? hi : lo) = mid;
        }
        const double res = 0.5 * (lo + hi);
        Solution t = solve(loop(res), f);
        printf("square loop: resonant perimeter %.4f lambda, R = %.1f\n", res, t.zin.real());
        CHECK(res > 1.0 && res < 1.1);
        CHECK(t.zin.real() > 95 && t.zin.real() < 150);
        Pattern p = pattern(loop(res), t, 5, 10);
        CHECK(p.maxDbi > 2.8 && p.maxDbi < 3.8);   // about 3.1-3.4 dBi broadside
    }
    // 2-element Yagi (driven + reflector at 0.15 lambda, free space): 6-8 dBi forward (+x), a front to back of several dB
    {
        Model m;
        Wire r; r.a = {0, -0.25 * lam, 0}; r.b = {0, 0.25 * lam, 0}; r.radius = 1e-3 * lam;
        Wire d; d.a = {0.15 * lam, -0.237 * lam, 0}; d.b = {0.15 * lam, 0.237 * lam, 0}; d.radius = 1e-3 * lam;
        m.wires = {r, d}; m.sources.push_back({1, 0.5, 1.0}); m.segPerWavelength = 30;
        Solution s = solve(m, f);
        Pattern p = pattern(m, s, 5, 5);
        Figures fg = figures(m, s, p);
        printf("2-el Yagi: %.2f dBi at phi %.0f, F/B %.1f dB, Z = %.1f %+.1fj\n", fg.maxDbi, fg.azMaxDeg, fg.fbDb, s.zin.real(), s.zin.imag());
        CHECK(fg.maxDbi > 6.0 && fg.maxDbi < 8.0);
        CHECK(std::fabs(fg.azMaxDeg) < 1 || std::fabs(fg.azMaxDeg - 360) < 1);
        CHECK(fg.fbDb > 5);
        CHECK(s.zin.real() > 10 && s.zin.real() < 60);
        // the template Yagi points the same way
        auto prm = templateParams("yagi", f);
        Model y = buildTemplate("yagi", f, prm);
        Solution ys = solve(y, f);
        Pattern yp = pattern(y, ys, 5, 5);
        CHECK(yp.maxDbi > 6.5);
        CHECK(std::cos(yp.maxPhi * kPi / 180) > 0.9);
    }
    // sweep: the dipole's reactance rises with frequency (Foster), the SWR minimum near resonance
    {
        Model m = dipole(0.48, 1e-4, f);
        std::vector<SweepPoint> sw = sweep(m, 0.8 * f, 1.2 * f, 21, 72);
        bool mono = true;
        for (size_t i = 1; i < sw.size(); ++i) { CHECK(sw[i].ok); if (sw[i].z.imag() <= sw[i - 1].z.imag()) mono = false; }
        CHECK(mono);
        size_t best = 0;
        for (size_t i = 1; i < sw.size(); ++i) if (sw[i].swr < sw[best].swr) best = i;
        CHECK(best > 4 && best < 16);
        CHECK(sw[best].swr < 1.5);
    }
    // segment refinement converges: the change halves (or better) each time the density doubles
    {
        Model m = dipole(0.48, 1e-4, f);
        double prev = 0, prevChange = 1e9;
        for (int spw : {10, 20, 40, 80}) {
            m.segPerWavelength = spw;
            const double r = solve(m, f).zin.real();
            if (prev > 0) { const double ch = std::fabs(r - prev); CHECK(ch < prevChange); prevChange = ch; }
            prev = r;
        }
        CHECK(prevChange < 1.0);
        Convergence c = convergence(m, f);
        CHECK(c.converged);
    }
    // lumped loads and lossy wire: a 50-ohm series resistor adds 50 ohm; copper costs a little efficiency, steel more
    {
        Model m = dipole(0.48, 1e-4, f);
        const double r0 = solve(m, f).zin.real();
        Load l; l.wire = 0; l.pos = 0.5; l.type = LoadType::RX; l.r = 50; m.loads.push_back(l);
        NEAR(solve(m, f).zin.real(), r0 + 50, 0.01);
        m.loads.clear();
        Model cu = m; cu.wires[0].conductivity = 5.8e7;
        Solution sc = solve(cu, f);
        CHECK(sc.pLoss > 0);
        Pattern pc = pattern(cu, sc, 5, 10);
        CHECK(pc.efficiency < 1.0 && pc.efficiency > 0.97);
        NEAR(pc.efficiency, 1 - sc.pLoss / sc.pIn, 0.01);
        Model st = m; st.wires[0].conductivity = 1e6; st.wires[0].radius = 1e-4;
        CHECK(pattern(st, solve(st, f), 5, 10).efficiency < pc.efficiency);
        // a parallel RLC trap at its resonance is (almost) an open circuit
        Model tr = dipole(0.48, 1e-4, f);
        Load t; t.wire = 0; t.pos = 0.5; t.type = LoadType::ParallelRLC; t.l = 1e-6; t.c = 1.0 / (std::pow(2 * kPi * f, 2) * 1e-6);
        tr.loads.push_back(t);
        CHECK(std::abs(solve(tr, f).zin) > 1e5);
    }
    // transmission line from a source to the dipole: half a wavelength repeats the impedance, a quarter inverts it
    {
        Model m = dipole(0.48, 1e-4, f);
        const cd zd = solve(m, f).zin;
        Model h = m; h.sources[0].line = 0;
        TLine tl; tl.wire1 = 0; tl.pos1 = 0.5; tl.z0 = 300; tl.length = lam / 2; tl.vf = 1; h.lines.push_back(tl);
        const cd zh = solve(h, f).zin;
        NEAR(zh.real(), zd.real(), 0.5); NEAR(zh.imag(), zd.imag(), 0.5);
        h.lines[0].length = lam / 4;
        const cd zq = solve(h, f).zin, want = 300.0 * 300.0 / zd;
        NEAR(zq.real(), want.real(), 0.02 * std::abs(want)); NEAR(zq.imag(), want.imag(), 0.02 * std::abs(want));
        // the same line, but with velocity factor 0.66 and the physical length 0.66 * lambda / 4: the same
        h.lines[0].length = 0.66 * lam / 4; h.lines[0].vf = 0.66;
        const cd zv = solve(h, f).zin;
        NEAR(zv.real(), want.real(), 0.02 * std::abs(want));
        // and the calculator agrees
        const cd zc = lineTransform(zd, 300, lam / 4, f, 1);
        NEAR(zc.real(), want.real(), 1e-6 * std::abs(want));
    }
    // two equal sources on two separate dipoles: each sees the mutual impedance, the power adds
    {
        Model m = dipole(0.48, 1e-4, f);
        Wire w2 = m.wires[0]; w2.a.x = w2.b.x = 0.5 * lam; m.wires.push_back(w2);
        m.sources.push_back({1, 0.5, 1.0});
        Solution s = solve(m, f);
        CHECK(s.srcZ.size() == 2);
        NEAR(s.srcZ[0].real(), s.srcZ[1].real(), 1e-6);
    }
    // near field: on the axis far out, |E| = the far field / r
    {
        Model m = dipole(0.48, 1e-4, f);
        Solution s = solve(m, f);
        const double r = 30 * lam;
        NearE e = nearField(m, s, {r, 0, 0});
        FarField ff = farField(m, s, 90, 0);
        const double en = std::sqrt(std::norm(e.ex) + std::norm(e.ey) + std::norm(e.ez));
        const double ef = std::sqrt(std::norm(ff.eth) + std::norm(ff.eph)) / r;
        NEAR(en / ef, 1.0, 0.02);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("antenna solver: all checks passed\n");
    return 0;
}
