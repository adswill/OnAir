// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ITU-R P.1812-8 (09/2025), written from the recommendation text; see p1812.h. Equation numbers refer to P.1812-8.
#include "dect2/prop/p1812.h"
#include <algorithm>
#include <cmath>

namespace dect2::prop::p1812 {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kEarth = 6371.0;

// 4.3.1: Bullington loss of profile heights g with terminals hts, hrs (m above sea level), effective radius ap (km)
double bullington(const std::vector<double>& d, const std::vector<double>& g, double hts, double hrs, double ap, double lam) {
    const size_t n = d.size();
    const double D = d.back(), Ce = 1.0 / ap;
    double Stim = -1e300;
    for (size_t i = 1; i + 1 < n; i++) Stim = std::max(Stim, (g[i] + 500 * Ce * d[i] * (D - d[i]) - hts) / d[i]);   // eq. (13)
    const double Str = (hrs - hts) / D;                                                                              // eq. (14)
    double Luc;
    if (Stim < Str) {   // case 1: LoS for diffraction
        double vmax = -1e300;
        for (size_t i = 1; i + 1 < n; i++)
            vmax = std::max(vmax, (g[i] + 500 * Ce * d[i] * (D - d[i]) - (hts * (D - d[i]) + hrs * d[i]) / D) *
                                      std::sqrt(0.002 * D / (lam * d[i] * (D - d[i]))));                             // eq. (15)
        Luc = knifeEdgeJ(vmax);
    } else {            // case 2: trans-horizon for diffraction
        double Srim = -1e300;
        for (size_t i = 1; i + 1 < n; i++) Srim = std::max(Srim, (g[i] + 500 * Ce * d[i] * (D - d[i]) - hrs) / (D - d[i]));   // eq. (17)
        const double dbp = (hrs - hts + Srim * D) / (Stim + Srim);                                                     // eq. (18)
        const double vb = (hts + Stim * dbp - (hts * (D - dbp) + hrs * dbp) / D) * std::sqrt(0.002 * D / (lam * dbp * (D - dbp)));   // eq. (19)
        Luc = knifeEdgeJ(vb);
    }
    return Luc + (1 - std::exp(-Luc / 6.0)) * (10 + 0.02 * D);                                                         // eq. (21)
}

// 4.3.3: first-term spherical-earth diffraction for one set of ground constants
double firstTerm1(double adft, double d, double ht, double hr, double f, double er, double sg, bool vert) {
    const double KH = 0.036 * std::pow(adft * f, -1.0 / 3.0) * std::pow((er - 1) * (er - 1) + (18 * sg / f) * (18 * sg / f), -0.25);   // eq. (29a)
    const double K = vert ? KH * std::sqrt(er * er + (18 * sg / f) * (18 * sg / f)) : KH;                                         // eq. (29b)
    const double K2 = K * K, K4 = K2 * K2;
    const double b = (1 + 1.6 * K2 + 0.67 * K4) / (1 + 4.5 * K2 + 1.53 * K4);                                                   // eq. (30)
    const double X = 21.88 * b * std::cbrt(f / (adft * adft)) * d;                                                              // eq. (31)
    const double yk = 0.9575 * b * std::cbrt(f * f / adft);
    const double FX = X >= 1.6 ? 11 + 10 * std::log10(X) - 17.6 * X : -20 * std::log10(X) - 5.6488 * std::pow(X, 1.425);      // eq. (33)
    auto G = [&](double Y) {
        const double B = b * Y;
        const double v = B > 2 ? 17.6 * std::sqrt(B - 1.1) - 5 * std::log10(B - 1.1) - 8 : 20 * std::log10(B + 0.1 * B * B * B);   // eq. (34)
        return std::max(v, 2 + 20 * std::log10(K));
    };
    return -FX - G(yk * ht) - G(yk * hr);                                                                                      // eq. (36)
}
double firstTerm(double adft, double d, double ht, double hr, double f, double w, bool vert) {   // eq. (28)
    return w * firstTerm1(adft, d, ht, hr, f, 80.0, 5.0, vert) + (1 - w) * firstTerm1(adft, d, ht, hr, f, 22.0, 0.003, vert);
}
// 4.3.2: spherical-earth diffraction loss
double sphericalEarth(double ap, double d, double ht, double hr, double f, double lam, double w, bool vert) {
    ht = std::max(ht, 0.01); hr = std::max(hr, 0.01);
    const double dlos = std::sqrt(2 * ap) * (std::sqrt(0.001 * ht) + std::sqrt(0.001 * hr));                                // eq. (22)
    if (d >= dlos) return firstTerm(ap, d, ht, hr, f, w, vert);
    const double c = (ht - hr) / (ht + hr), m = 250 * d * d / (ap * (ht + hr));                                              // eq. (24d, e)
    const double b = 2 * std::sqrt((m + 1) / (3 * m)) *
                     std::cos(kPi / 3 + std::acos(std::clamp(3 * c / 2 * std::sqrt(3 * m / ((m + 1) * (m + 1) * (m + 1))), -1.0, 1.0)) / 3);   // (24c)
    const double d1 = d / 2 * (1 + b), d2 = d - d1;
    const double hse = ((ht - 500 * d1 * d1 / ap) * d2 + (hr - 500 * d2 * d2 / ap) * d1) / d;                               // eq. (23)
    const double hreq = 17.456 * std::sqrt(d1 * d2 * lam / d);                                                              // eq. (25)
    if (hse > hreq) return 0;
    const double s = std::sqrt(ht) + std::sqrt(hr);
    const double aem = 500 * (d / s) * (d / s);                                                                              // eq. (26)
    const double L = firstTerm(aem, d, ht, hr, f, w, vert);
    return L < 0 ? 0 : (1 - hse / hreq) * L;                                                                                // eq. (27)
}
}

double knifeEdgeJ(double v) {
    if (v <= -0.78) return 0;
    return 6.9 + 20 * std::log10(std::sqrt((v - 0.1) * (v - 0.1) + 1) + v - 0.1);
}

double invQ(double x) {
    x = std::clamp(x, 1e-6, 1 - 1e-6);
    if (x > 0.5) return -invQ(1 - x);
    const double T = std::sqrt(-2 * std::log(x));
    const double xi = ((0.010328 * T + 0.802853) * T + 2.515516698) / (((0.001308 * T + 0.189269) * T + 1.432788) * T + 1);
    return T - xi;
}

double sigmaLocationDb(double fGHz, double wa) { return (0.024 * fGHz + 0.52) * std::pow(std::max(wa, 1.0), 0.28); }

double heightVariation(double h, double R) {
    if (R <= 0) return h >= 10 ? 0.0 : 1.0 - h / 10;
    if (h < R) return 1.0;
    if (h < R + 10) return 1.0 - (h - R) / 10;
    return 0.0;
}

Result compute(const Input& in) {
    Result o;
    const size_t n = std::min(in.dKm.size(), in.hM.size());
    if (n < 3 || !(in.dKm[n - 1] > 0) || !(in.fGHz > 0)) return o;
    std::vector<double> d(in.dKm.begin(), in.dKm.begin() + n), h(in.hM.begin(), in.hM.begin() + n), g(h);
    for (size_t i = 1; i + 1 < n && i < in.clutterM.size(); i++) g[i] = h[i] + in.clutterM[i];   // eq. (1d)
    const double D = d[n - 1], f = in.fGHz, lam = 0.299792458 / f;
    const double p = std::clamp(in.timePct, 1.0, 50.0), w = std::clamp(in.seaFraction, 0.0, 1.0);
    const double hts = h[0] + in.txHeightM, hrs = h[n - 1] + in.rxHeightM;
    o.dKm = D; o.hts = hts; o.hrs = hrs;
    // 3.6 / 3.7: beta0 and the effective earth radius
    const double dtm = in.dtmKm >= 0 ? in.dtmKm : D, dlm = in.dlmKm >= 0 ? in.dlmKm : D;
    const double tau = 1 - std::exp(-4.12e-4 * std::pow(dlm, 2.41));                                                        // eq. (3)
    const double mu1 = std::min(1.0, std::pow(std::pow(10.0, -dtm / (16 - 6.6 * tau)) + std::pow(10.0, -5 * (0.496 + 0.354 * tau)), 0.2));   // eq. (2)
    const double alat = std::fabs(in.latDeg);
    double mu4, b0;
    if (alat <= 70) { mu4 = std::pow(10.0, (-0.935 + 0.0176 * alat) * std::log10(mu1)); b0 = std::pow(10.0, -0.015 * alat + 1.67) * mu1 * mu4; }
    else { mu4 = std::pow(10.0, 0.3 * std::log10(mu1)); b0 = 4.17 * mu1 * mu4; }                                           // eqs. (4), (5)
    const double k50 = 157.0 / (157.0 - std::clamp(in.deltaN, 0.0, 150.0));                                                // eq. (6)
    const double ae = k50 * kEarth, ab = 3.0 * kEarth;                                                                     // eq. (7)
    o.ae = ae; o.beta0 = b0;
    // Attachment 1, section 4-5: path classification and horizons (terrain heights)
    const double thtd = 1000 * std::atan((hrs - hts) / (1e3 * D) - D / (2 * ae));                                          // eq. (76)
    double thmax = -1e300; size_t imax = 1;
    for (size_t i = 1; i + 1 < n; i++) {
        const double th = 1000 * std::atan((h[i] - hts) / (1e3 * d[i]) - d[i] / (2 * ae));                                // eq. (75)
        if (th > thmax) { thmax = th; imax = i; }
    }
    size_t ilt, ilr;
    if (thmax > thtd) {   // trans-horizon
        o.los = false;
        o.tht = thmax; ilt = imax; o.dlt = d[ilt];                                                                         // eqs. (77), (78)
        double thr = -1e300; size_t jr = 1;
        for (size_t j = 1; j + 1 < n; j++) {
            const double th = 1000 * std::atan((h[j] - hrs) / (1e3 * (D - d[j])) - (D - d[j]) / (2 * ae));                 // eq. (80a)
            if (th > thr) { thr = th; jr = j; }
        }
        o.thr = thr; ilr = jr; o.dlr = D - d[jr];                                                                          // eq. (81)
    } else {               // LoS: the point with the largest diffraction parameter
        o.tht = thtd;
        const double Ce = 1.0 / ae;
        double vmax = -1e300; size_t iv = 1;
        for (size_t i = 1; i + 1 < n; i++) {
            const double v = (h[i] + 500 * Ce * d[i] * (D - d[i]) - (hts * (D - d[i]) + hrs * d[i]) / D) * std::sqrt(0.002 * D / (lam * d[i] * (D - d[i])));   // (78a)
            if (v > vmax) { vmax = v; iv = i; }
        }
        ilt = ilr = iv; o.dlt = d[iv]; o.dlr = D - o.dlt;                                                                  // eq. (81a)
        o.thr = 1000 * std::atan((hts - hrs) / (1e3 * D) - D / (2 * ae));                                                 // eq. (79)
    }
    o.theta = 1e3 * D / ae + o.tht + o.thr;                                                                                // eq. (82)
    // 5.6: smooth earth
    double v1 = 0, v2 = 0;
    for (size_t i = 1; i < n; i++) {
        v1 += (d[i] - d[i - 1]) * (h[i] + h[i - 1]);                                                                       // eq. (83)
        v2 += (d[i] - d[i - 1]) * (h[i] * (2 * d[i] + d[i - 1]) + h[i - 1] * (d[i] + 2 * d[i - 1]));                       // eq. (84)
    }
    const double hst0 = (2 * v1 * D - v2) / (D * D), hsr0 = (v2 - v1 * D) / (D * D);                                       // eqs. (85), (86)
    double hobs = -1e300, aobt = -1e300, aobr = -1e300;
    for (size_t i = 1; i + 1 < n; i++) {
        const double Hi = h[i] - (hts * (D - d[i]) + hrs * d[i]) / D;                                                      // eq. (87d)
        hobs = std::max(hobs, Hi); aobt = std::max(aobt, Hi / d[i]); aobr = std::max(aobr, Hi / (D - d[i]));
    }
    double hstp = hst0, hsrp = hsr0;
    if (hobs > 0) { const double gt = aobt / (aobt + aobr), gr = aobr / (aobt + aobr); hstp = hst0 - hobs * gt; hsrp = hsr0 - hobs * gr; }   // (88)
    o.hstd = hstp > h[0] ? h[0] : hstp;                                                                                    // eq. (89)
    o.hsrd = hsrp > h[n - 1] ? h[n - 1] : hsrp;
    const double hst = std::min(hst0, h[0]), hsr = std::min(hsr0, h[n - 1]), m = (hsr - hst) / D;                          // eqs. (90), (91)
    o.hte = in.txHeightM + h[0] - hst; o.hre = in.rxHeightM + h[n - 1] - hsr;                                              // eq. (92)
    o.hm = 0;
    for (size_t i = ilt; i <= ilr && i < n; i++) o.hm = std::max(o.hm, h[i] - (hst + m * d[i]));                          // eq. (93)
    // 4.2: line of sight
    const double dfs = std::sqrt(D * D + ((hts - hrs) / 1000) * ((hts - hrs) / 1000));
    o.lbfs = 92.4 + 20 * std::log10(f) + 20 * std::log10(dfs);                                                             // eq. (8)
    const double ek = 2.6 * (1 - std::exp(-0.1 * (o.dlt + o.dlr)));
    o.lb0p = o.lbfs + ek * std::log10(p / 50);                                                                             // eqs. (9a), (10)
    o.lb0b = o.lbfs + ek * std::log10(b0 / 50);                                                                            // eqs. (9b), (11)
    // 4.3: delta-Bullington diffraction for a given effective radius
    const std::vector<double> zero(n, 0.0);
    const double htc2 = hts - o.hstd, hrc2 = hrs - o.hsrd;                                                                 // eq. (37)
    auto Ld = [&](double ap, double& la, double& ls, double& lsph) {
        la = bullington(d, g, hts, hrs, ap, lam);
        ls = bullington(d, zero, htc2, hrc2, ap, lam);
        lsph = sphericalEarth(ap, D, htc2, hrc2, f, lam, w, in.vertical);
        return la + std::max(lsph - ls, 0.0);                                                                              // eq. (39)
    };
    o.ld50 = Ld(ae, o.lbulla, o.lbulls, o.ldsph);
    double t1, t2, t3;
    o.ldb = Ld(ab, t1, t2, t3);
    double Fi;                                                                                                              // eq. (40)
    if (p >= 50) Fi = 0;
    else if (p > b0) Fi = invQ(p / 100) / invQ(b0 / 100);
    else Fi = 1;
    o.ldp = o.ld50 + (o.ldb - o.ld50) * Fi;                                                                               // eq. (41)
    const double lbd50 = o.lbfs + o.ld50;                                                                                  // eq. (42)
    o.lbd = o.lb0p + o.ldp;                                                                                                // eq. (43)
    // 4.4: troposcatter
    const double Lf = 25 * std::log10(f) - 2.5 * std::pow(std::log10(f / 2), 2);                                          // eq. (45)
    o.lbs = 190.1 + Lf + 20 * std::log10(D) + 0.573 * o.theta - 0.15 * in.n0 - 10.125 * std::pow(std::log10(50 / p), 0.7);   // eq. (44)
    // 4.5: ducting / layer reflection
    const double Alf = f < 0.5 ? 45.375 - 137.0 * f + 92.5 * f * f : 0;                                                   // eq. (47a)
    auto site = [&](double th, double dl) {                                                                                // eq. (48)
        const double t = th - 0.1 * dl;                                                                                    // eq. (48a)
        return t > 0 ? 20 * std::log10(1 + 0.361 * t * std::sqrt(f * dl)) + 0.264 * t * std::cbrt(f) : 0.0;
    };
    const double Af = 102.45 + 20 * std::log10(f) + 20 * std::log10(std::max(o.dlt + o.dlr, 1e-3)) + Alf + site(o.tht, o.dlt) + site(o.thr, o.dlr);   // (47)
    const double gd = 5e-5 * ae * std::cbrt(f);                                                                            // eq. (51)
    const double tht2 = o.tht <= 0.1 * o.dlt ? o.tht : 0.1 * o.dlt, thr2 = o.thr <= 0.1 * o.dlr ? o.thr : 0.1 * o.dlr;      // eq. (52a)
    const double thp = 1e3 * D / ae + tht2 + thr2;                                                                         // eq. (52)
    const double alpha = std::max(-0.6 - 3.5e-9 * std::pow(D, 3.1) * tau, -3.4);                                           // eq. (55a)
    const double sh = std::sqrt(std::max(o.hte, 0.01)) + std::sqrt(std::max(o.hre, 0.01));
    const double mu2 = std::min(1.0, std::pow(500 / ae * D * D / (sh * sh), alpha));                                       // eq. (55)
    const double dI = std::min(D - o.dlt - o.dlr, 40.0);                                                                   // eq. (56a)
    const double mu3 = o.hm <= 10 ? 1.0 : std::exp(-4.6e-5 * (o.hm - 10) * (43 + 6 * dI));                                 // eq. (56)
    const double beta = b0 * mu2 * mu3;                                                                                    // eq. (54)
    const double lb_ = std::log10(beta);
    const double Gam = 1.076 / std::pow(2.0058 - lb_, 1.012) * std::exp(-(9.51 - 4.8 * lb_ + 0.198 * lb_ * lb_) * 1e-6 * std::pow(D, 1.13));   // (53a)
    const double Ap = -12 + (1.2 + 3.7e-3 * D) * std::log10(p / beta) + 12 * std::pow(p / beta, Gam);                     // eq. (53)
    o.lba = Af + gd * thp + Ap;                                                                                            // eqs. (46), (50)
    // 4.6: blend
    const double Fj = 1 - 0.5 * (1 + std::tanh(3 * 0.8 * (o.theta - 0.3) / 0.3));                                         // eq. (57)
    const double Fk = 1 - 0.5 * (1 + std::tanh(3 * 0.5 * (D - 20) / 20));                                                 // eq. (58)
    const double lminb0p = p < b0 ? o.lb0p + (1 - w) * o.ldp : lbd50 + (o.lb0b + (1 - w) * o.ldp - lbd50) * Fi;           // eq. (59)
    const double eta = 2.5;
    const double mx = std::max(o.lba, o.lb0p) / eta;
    const double lminbap = eta * (mx + std::log(std::exp(o.lba / eta - mx) + std::exp(o.lb0p / eta - mx)));             // eq. (60), overflow-safe
    const double lbda = lminbap > o.lbd ? o.lbd : lminbap + (o.lbd - lminbap) * Fk;                                       // eq. (61)
    const double lbam = lbda + (lminb0p - lbda) * Fj;                                                                      // eq. (62)
    const double mn = std::min(o.lbs, lbam);
    o.lbc = mn - 5 * std::log10(std::pow(10.0, -0.2 * (o.lbs - mn)) + std::pow(10.0, -0.2 * (lbam - mn)));                  // eq. (63)
    // 4.7 / 4.9: locations
    o.sigmaLoc = std::max(in.sigmaLocDb, 0.0);
    const double pl = std::clamp(in.locPct, 1.0, 99.0) / 100.0;
    o.lb = std::max(o.lb0p, o.lbc - invQ(pl) * o.sigmaLoc);                                                                // eq. (69)
    // which mechanism carries most of the power
    if (o.lbs < lbam - 3) o.mechanism = 2;
    else if (o.lba < o.lbd - 3) o.mechanism = 3;
    else o.mechanism = o.ldp > 1.0 ? 1 : 0;
    o.valid = true;
    return o;
}

} // namespace dect2::prop::p1812
