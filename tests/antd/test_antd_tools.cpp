// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Antenna designer tools: every template builds and solves to a sane impedance, the calculators against hand-worked values, the NEC deck
// and pattern file round trips, and the optimiser trimming a dipole to resonance.
#include "dect2/antd/antd.h"
#include "dect2/antd/antd_tools.h"
#include <cmath>
#include <cstdio>
using namespace dect2::antd;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (std::fabs(_a - _b) > (tol)) { printf("FAIL line %d: %s = %.4f, want %.4f +- %g\n", __LINE__, #a, _a, _b, (double)(tol)); fails++; } } while (0)

int main() {
    // templates: build, check, solve at the design frequency
    struct Expect { const char* id; double f; double rlo, rhi; };
    const Expect ex[] = {
        {"dipole", 14.2e6, 40, 110}, {"invv", 7.1e6, 25, 90}, {"folded", 145e6, 200, 380}, {"ocf", 7.1e6, 80, 400},
        {"efhw", 14.2e6, 300, 8000}, {"g5rv", 14.2e6, 5, 2000}, {"delta", 14.2e6, 50, 250}, {"quadloop", 145e6, 70, 200},
        {"magloop", 7.1e6, 25, 100}, {"yagi", 145e6, 5, 80}, {"quad", 28.5e6, 40, 200}, {"vertical", 14.2e6, 15, 60},
        {"groundplane", 145e6, 25, 80}, {"jpole", 145e6, 30, 90}, {"slimjim", 145e6, 1, 3000}, {"helix", 435e6, 30, 400},
        {"turnstile", 137.5e6, 10, 150}, {"lpda", 200e6, 10, 300}, {"discone", 145e6, 5, 300},
    };
    for (const Expect& e : ex) {
        CHECK(findTemplate(e.id) != nullptr);
        std::string note;
        const auto prm = templateParams(e.id, e.f);
        CHECK(!prm.empty());
        Model m = buildTemplate(e.id, e.f, prm, &note);
        CHECK(!m.wires.empty() && !m.sources.empty());
        Solution s = solve(m, e.f);
        if (!s.ok) printf("  %s: %s\n", e.id, s.error.c_str());
        CHECK(s.ok);
        const double r = s.zin.real();
        printf("%-12s %7.3f MHz: %4zu segments  Z = %8.1f %+8.1fj\n", e.id, e.f / 1e6, s.mesh.segs.size(), r, s.zin.imag());
        if (!(r > e.rlo && r < e.rhi)) { printf("FAIL %s: R %.1f outside %.0f..%.0f\n", e.id, r, e.rlo, e.rhi); fails++; }
    }
    // the ground plane: about 50 ohm with 45-degree radials; the helix beams up with circular polarisation
    {
        const double f = 145e6;
        Model m = buildTemplate("groundplane", f, templateParams("groundplane", f));
        Solution s = solve(m, f);
        CHECK(std::fabs(s.zin.real() - 50) < 20);
        Model h = buildTemplate("helix", 435e6, templateParams("helix", 435e6));
        Solution hs = solve(h, 435e6);
        Pattern p = pattern(h, hs, 5, 15);
        CHECK(p.maxTheta < 20);
        CHECK(p.maxDbi > 9);
        const Gain g = gainAt(h, hs, 0, 0);
        CHECK(std::fabs(g.vert - g.horiz) < 3);   // both linear parts about equal overhead: circular
    }
    // calculators
    NEAR(wavelength(300e6), 0.99930819, 1e-6);
    NEAR(wireLengths(14.2e6).halfDipole, 10.046, 0.01);
    NEAR(wireLengths(7.1e6).quarterVertical, 10.042, 0.01);
    {
        const Mismatch m = fromSwr(2.0);
        NEAR(m.gamma, 1.0 / 3, 1e-9); NEAR(m.returnLossDb, 9.542, 0.001); NEAR(m.mismatchLossDb, 0.512, 0.001); NEAR(m.reflectedPct, 11.11, 0.01);
        const Mismatch z = fromZ(cd(100, 0), 50);
        NEAR(z.swr, 2.0, 1e-9);
        NEAR(fromReturnLoss(20).swr, 1.2222, 1e-3);
    }
    {
        // 3 dB matched loss, SWR 3 at the load: total 3.97 dB, SWR 1.67 at the input
        const LineLoss l = mismatchedLineLoss(3.0, 3.0);
        NEAR(l.totalDb, 3.97, 0.02); NEAR(l.swrIn, 1.67, 0.02);
        const Cable* rg58 = nullptr;
        for (const auto& c : cables()) if (std::string(c.name) == "RG-58") rg58 = &c;
        CHECK(rg58 != nullptr);
        if (rg58) { NEAR(cableLossDb(*rg58, 100e6, 100), 16, 1.5); NEAR(cableLossDb(*rg58, 10e6, 10), 0.46, 0.05); }
    }
    {
        // L network: 200 ohm to 50 ohm at 14 MHz: series X = +-86.6, shunt X = -+115.5 (Q = sqrt(3))
        const auto ln = lNetwork(cd(200, 0), 50, 14e6);
        CHECK(ln.size() == 2);
        for (const auto& m : ln) {
            NEAR(std::fabs(m.xSeries), 86.60, 0.05); NEAR(std::fabs(m.xShunt), 115.47, 0.05);
            CHECK(m.shuntAtLoad);
            // check it: the shunt across the load, then the series
            const cd zp = 1.0 / (1.0 / cd(200, 0) + 1.0 / cd(0, m.xShunt));
            const cd zin = zp + cd(0, m.xSeries);
            NEAR(zin.real(), 50, 1e-6); NEAR(zin.imag(), 0, 1e-6);
        }
        const auto lo = lNetwork(cd(10, 5), 50, 7e6);
        for (const auto& m : lo) {
            const cd zs = cd(10, 5) + cd(0, m.xSeries);
            const cd zin = 1.0 / (1.0 / zs + 1.0 / cd(0, m.xShunt));
            NEAR(zin.real(), 50, 1e-6); NEAR(zin.imag(), 0, 1e-6);
        }
        CHECK(reactancePart(100, 1e6).find("uH") != std::string::npos);
        CHECK(reactancePart(-100, 1e6).find("nF") != std::string::npos);
    }
    {
        // shunt stub: verify by transforming the load d along the line and adding the stub's admittance
        const double f = 100e6, lam = kC0 / f;
        for (bool open : {false, true}) {
            const cd zl(25, -30);
            for (const auto& s : stubMatch(zl, 50, f, 1, open)) {
                const cd zd = lineTransform(zl, 50, s.dM, f, 1);
                const cd zs = open ? cd(0, -50 / std::tan(2 * kPi * s.lLambda)) : cd(0, 50 * std::tan(2 * kPi * s.lLambda));
                const cd zin = 1.0 / (1.0 / zd + 1.0 / zs);
                NEAR(zin.real(), 50, 1e-3); NEAR(zin.imag(), 0, 1e-3);
                CHECK(s.dM >= 0 && s.dM <= lam / 2 + 1e-9);
            }
        }
        const QuarterWave q = quarterWave(100, 50, f, 0.66);
        NEAR(q.z, 70.71, 0.01); NEAR(q.lengthM, 0.66 * lam / 4, 1e-9);
        NEAR(balunChoke(7e6, 2450, 50).turnsRatio, 7, 1e-9);
        double rp, xp; seriesToParallel(cd(10, 20), rp, xp);
        NEAR(rp, 50, 1e-9); NEAR(xp, 25, 1e-9);
        const cd zz = parallelToSeries(50, 25);
        NEAR(zz.real(), 10, 1e-9); NEAR(zz.imag(), 20, 1e-9);
    }
    {
        const Eirp e = eirp(100, 3, 8.15);
        NEAR(e.eirpDbm, 55.15, 1e-9); NEAR(e.erpDbm, 53.0, 1e-9);
        NEAR(antennaFactorDb(100e6, 0), 20 * std::log10(100.0) - 29.77, 0.05);   // 50 ohm: 20 log f - G - 29.77
        NEAR(gainFromAperture(1.0, 1e9), 10 * std::log10(4 * kPi / std::pow(kC0 / 1e9, 2)), 1e-9);
        CHECK(bandPresets().size() > 20);
    }
    // NEC deck round trip: the same impedance back
    {
        const double f = 14.2e6;
        Model m = buildTemplate("dipole", f, templateParams("dipole", f));
        Load l; l.wire = 0; l.pos = 0.5; l.type = LoadType::RX; l.r = 5; m.loads.push_back(l);
        const std::string deck = writeNec(m, 14e6, 14.35e6, 8);
        CHECK(deck.find("GW 1") != std::string::npos && deck.find("EX 0 1") != std::string::npos && deck.find("GN 0") != std::string::npos);
        Model back; double f0 = 0, f1 = 0; int nf = 0; std::string err;
        CHECK(readNec(deck, back, &f0, &f1, &nf, &err));
        NEAR(f0, 14e6, 1); NEAR(f1, 14.35e6, 10); CHECK(nf == 8);
        CHECK(back.wires.size() == 1 && back.sources.size() == 1 && back.loads.size() == 1 && back.ground.type == GroundType::Real);
        const cd za = solve(m, f).zin, zb = solve(back, f).zin;
        NEAR(zb.real(), za.real(), 0.03 * std::abs(za)); NEAR(zb.imag(), za.imag(), 0.03 * std::abs(za) + 1);
        // a hand-written deck: a monopole on a perfect ground (GN 1), fed at the base, with a GM copy and a GH helix
        const char* d2 =
            "CM test\nCE\nGW 1 10 0 0 0 0 0 0.5 0.001\nGW 2 5 0.3 0 0 0.3 0 0.2 0.001\nGM 0 0 0 0 90 0 0 0 2\nGE 1\nGN 1\nEX 0 1 1 0 1 0\nFR 0 1 0 0 150 0\nEN\n";
        Model m2;
        CHECK(readNec(d2, m2, &f0, nullptr, nullptr, &err));
        CHECK(m2.wires.size() == 2 && m2.ground.type == GroundType::Perfect);
        NEAR(m2.sources[0].pos, 0, 1e-12);
        NEAR(m2.wires[1].a.y, 0.3, 1e-9);   // rotated 90 degrees about z
        const char* d3 = "GH 1 40 0.15 0.6 0.08 0.08 0.08 0.08 0.001\nGE 0\nEN\n";
        Model m3;
        CHECK(readNec(d3, m3, nullptr, nullptr, nullptr, &err));
        CHECK(m3.wires.size() == 40);
        NEAR(m3.wires.back().b.z, 0.6, 1e-9);
        Model bad;
        CHECK(!readNec("SP 0 0 0 0 0 0\nEN\n", bad, nullptr, nullptr, nullptr, &err));
        // a source on a line end (G5RV) survives as a stub wire with the source
        Model g = buildTemplate("g5rv", f, templateParams("g5rv", f));
        const std::string dg = writeNec(g, f, f, 1);
        Model gb; CHECK(readNec(dg, gb, nullptr, nullptr, nullptr, &err));
        CHECK(gb.wires.size() == 2 && gb.lines.size() == 1 && gb.sources.size() == 1);
    }
    // the pattern file: written and read back, the gain at a direction matches the solver
    {
        const double f = 145e6;
        Model m = buildTemplate("yagi", f, templateParams("yagi", f));
        m.name = "test yagi";
        Solution s = solve(m, f);
        const std::string csv = writePatternCsv(m, s, 5, 5);
        PatternFile p; std::string err;
        CHECK(readPatternCsv(csv, p, &err));
        CHECK(p.nAz == 72 && p.nEl == 37 && p.name == "test yagi");
        NEAR(p.fMHz, 145, 1e-6);
        // compass bearing 90 = +x = phi 0: the beam
        NEAR(patternGainDbi(p, 90, 0), gainAt(m, s, 90, 0).total, 0.02);
        NEAR(patternGainDbi(p, 270, 0), gainAt(m, s, 90, 180).total, 0.02);
        NEAR(patternGainDbi(p, 92.5, 2.5), 0.25 * (gainAt(m, s, 90, 0).total + gainAt(m, s, 90, 355).total + gainAt(m, s, 85, 0).total + gainAt(m, s, 85, 355).total), 0.3);
        CHECK(p.maxDbi > 6);
        NEAR(patternGainDbi(p, 359.9, 45), patternGainDbi(p, 0, 45), 0.2);   // wraps
        CHECK(writeSweepCsv(sweep(m, 144e6, 146e6, 3, 50), 50).find("f_mhz") == 0);
        CHECK(writeCurrentsCsv(m, s).find("wire,segment") == 0);
    }
    // optimiser: a too-long dipole trimmed to SWR < 1.3 against 70 ohm
    {
        const double f = 100e6, lam = kC0 / f;
        auto prm = templateParams("dipole", f);
        for (auto& p : prm) if (p.key == "len") p.value = 0.53 * lam;
        OptGoal g; g.f0Hz = g.f1Hz = f; g.points = 1; g.z0 = 70; g.swrTarget = 1.1;
        Ground fs;
        const OptResult r = optimise("dipole", f, prm, {{"len", 0.4 * lam, 0.6 * lam}}, g, fs, 80);
        CHECK(r.ok);
        printf("optimiser: length %.4f lambda, SWR %.3f after %d solves\n", paramValue(r.params, "len") / lam, r.worstSwr, r.evals);
        CHECK(r.worstSwr < 1.3);
        CHECK(paramValue(r.params, "len") / lam > 0.46 && paramValue(r.params, "len") / lam < 0.5);
        // trim to resonance: the dipole's length until X = 0
        auto tp = templateParams("dipole", f);
        cd zt;
        CHECK(trimParam("dipole", f, tp, findTemplate("dipole")->trim, &fs, 50, &zt));
        NEAR(zt.imag(), 0, 0.5);
        CHECK(zt.real() > 60 && zt.real() < 80);
        // Nelder-Mead on the Rosenbrock valley
        auto rosen = [](const std::vector<double>& x) { return 100 * std::pow(x[1] - x[0] * x[0], 2) + std::pow(1 - x[0], 2); };
        const auto xb = nelderMead(rosen, {-1.2, 1}, {-3, -3}, {3, 3}, 2000, 1e-9);
        NEAR(xb[0], 1, 0.01); NEAR(xb[1], 1, 0.02);
    }
    // model helpers
    {
        Model m; Wire w; w.a = {1, 0, 0}; w.b = {2, 0, 0}; m.wires.push_back(w);
        mirror(m, 0, true);
        CHECK(m.wires.size() == 2); NEAR(m.wires[1].a.x, -1, 1e-12);
        arrayCopy(m, {0}, 3, {0, 1, 0});
        CHECK(m.wires.size() == 5); NEAR(m.wires[4].a.y, 3, 1e-12);
        rotateZ(m, 90);
        NEAR(m.wires[0].a.y, 1, 1e-12);
        CHECK(!checkModel(m, 100e6).empty());   // no source
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("antenna tools: all checks passed\n");
    return 0;
}
