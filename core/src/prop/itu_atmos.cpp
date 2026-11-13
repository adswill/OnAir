// Atmosphere of a radio path from ITU-R P.835-7, P.676-13, P.838-3, P.453-14, P.530-19, P.618-14 and P.839-4 (see itu_atmos.h).
// Written from the recommendation texts; the spectral line table is P.676-13 Annex 1 Tables 1 and 2, copied as published.
#include "dect2/prop/itu_atmos.h"
#include <algorithm>
#include <cmath>

namespace dect2::prop::itu {
namespace {
#include "p676_lines.inc"
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kEarthKm = 6371.0;
}

Atmos refAtmosphere(double hKm, double rho0) {
    // P.835-7 section 1.1, geopotential height h' (km'), eq. (1a)
    const double h = std::max(hKm, 0.0);
    const double hp = 6356.766 * h / (6356.766 + h);
    double T, P;
    if (hp <= 11) { T = 288.15 - 6.5 * hp; P = 1013.25 * std::pow(288.15 / T, -34.1632 / 6.5); }
    else if (hp <= 20) { T = 216.65; P = 226.3226 * std::exp(-34.1632 * (hp - 11) / 216.65); }
    else if (hp <= 32) { T = 216.65 + (hp - 20); P = 54.74980 * std::pow(216.65 / T, 34.1632); }
    else if (hp <= 47) { T = 228.65 + 2.8 * (hp - 32); P = 8.680422 * std::pow(228.65 / T, 34.1632 / 2.8); }
    else if (hp <= 51) { T = 270.65; P = 1.109106 * std::exp(-34.1632 * (hp - 47) / 270.65); }
    else if (hp <= 71) { T = 270.65 - 2.8 * (hp - 51); P = 0.6694167 * std::pow(270.65 / T, -34.1632 / 2.8); }
    else { const double hq = std::min(hp, 84.852); T = 214.65 - 2.0 * (hq - 71); P = 0.03956649 * std::pow(214.65 / T, -34.1632 / 2.0);
           if (hp > 84.852) P *= std::exp(-(hp - 84.852) / 6.0); }   // above 85 km: the P.835 upper formulas are not needed for attenuation
    double rho = rho0 * std::exp(-h / 2.0);                   // eq. (6), scale height 2 km
    const double e = vapourPressureHpa(rho, T);
    if (P > 0 && e / P < 2e-6) rho = 2e-6 * P * 216.7 / T;    // the mixing ratio does not fall below 2e-6
    Atmos a; a.pHpa = P; a.tK = T; a.rhoGm3 = rho;
    return a;
}

double gammaOxygenDbKm(double f, double p, double e, double T) {
    const double th = 300.0 / T;
    double n = 0;
    for (const auto& L : kO2) {
        const double fi = L[0];
        const double S = L[1] * 1e-7 * p * th * th * th * std::exp(L[2] * (1 - th));                       // eq. (3)
        double df = L[3] * 1e-4 * (p * std::pow(th, 0.8 - L[4]) + 1.1 * e * th);                         // eq. (6a)
        df = std::sqrt(df * df + 2.25e-6);                                                                 // eq. (6b), Zeeman
        const double dl = (L[5] + L[6] * th) * 1e-4 * (p + e) * std::pow(th, 0.8);                       // eq. (7)
        const double F = f / fi * ((df - dl * (fi - f)) / ((fi - f) * (fi - f) + df * df) +
                                   (df - dl * (fi + f)) / ((fi + f) * (fi + f) + df * df));               // eq. (5)
        n += S * F;
    }
    const double d = 5.6e-4 * (p + e) * std::pow(th, 0.8);                                                // eq. (9)
    const double nd = f * p * th * th * (6.14e-5 / (d * (1 + (f / d) * (f / d))) +
                                         1.4e-12 * p * std::pow(th, 1.5) / (1 + 1.9e-5 * std::pow(f, 1.5)));   // eq. (8)
    return 0.1820 * f * (n + nd);                                                                         // eq. (1)
}

double gammaWaterDbKm(double f, double p, double e, double T) {
    const double th = 300.0 / T;
    double n = 0;
    for (const auto& L : kH2O) {
        const double fi = L[0];
        const double S = L[1] * 1e-1 * e * std::pow(th, 3.5) * std::exp(L[2] * (1 - th));
        double df = L[3] * 1e-4 * (p * std::pow(th, L[4]) + L[5] * e * std::pow(th, L[6]));
        df = 0.535 * df + std::sqrt(0.217 * df * df + 2.1316e-12 * fi * fi / th);                        // Doppler broadening
        const double F = f / fi * (df / ((fi - f) * (fi - f) + df * df) + df / ((fi + f) * (fi + f) + df * df));
        n += S * F;
    }
    return 0.1820 * f * n;
}

double gammaGasDbKm(double f, double pTot, double T, double rho) {
    const double e = vapourPressureHpa(rho, T);
    const double p = std::max(pTot - e, 0.0);
    return gammaOxygenDbKm(f, p, e, T) + gammaWaterDbKm(f, p, e, T);
}

double terrestrialGasDb(double f, double dKm, double pTot, double T, double rho) { return gammaGasDbKm(f, pTot, T, rho) * std::max(dKm, 0.0); }

double refractivityN(double pTot, double T, double e) { return 77.6 / T * (pTot + 4810.0 * e / T); }

double slantGasDb(double f, double hs, double elevDeg, double rho0) {
    // P.676-13 Annex 1 section 2.2.1: layers of thickness 0.0001 exp((i-1)/100) km from the station to 100 km (eq. 14), the path length in
    // each (eq. 17), the angle at the top of the layer (eq. 18) and Snell's law into the next one (eq. 19)
    double beta = (90.0 - std::clamp(elevDeg, 0.0, 90.0)) * kDeg;
    double h = std::max(hs, 0.0), A = 0;
    Atmos at = refAtmosphere(h, rho0);
    double nCur = 1 + 1e-6 * refractivityN(at.pHpa, at.tK, vapourPressureHpa(at.rhoGm3, at.tK));
    for (int i = 1; h < 100.0 && i < 5000; i++) {
        const double dlt = 1e-4 * std::exp((i - 1) / 100.0);
        const double r = kEarthKm + h;
        const double cb = std::cos(beta);
        const double a = -r * cb + 0.5 * std::sqrt(4 * r * r * cb * cb + 8 * r * dlt + 4 * dlt * dlt);
        const Atmos m = refAtmosphere(h + dlt / 2, rho0);
        A += a * gammaGasDbKm(f, m.pHpa, m.tK, m.rhoGm3);
        const double ca = std::clamp((-a * a - 2 * r * dlt - dlt * dlt) / (2 * a * r + 2 * a * dlt), -1.0, 1.0);
        const double alpha = kPi - std::acos(ca);
        h += dlt;
        const Atmos up = refAtmosphere(h + 1e-4 * std::exp(i / 100.0) / 2, rho0);
        const double nNext = 1 + 1e-6 * refractivityN(up.pHpa, up.tK, vapourPressureHpa(up.rhoGm3, up.tK));
        beta = std::asin(std::clamp(nCur / nNext * std::sin(alpha), -1.0, 1.0));
        nCur = nNext;
    }
    return A;
}

namespace {
struct Gc { double a, b, c; };
// P.838-3 Tables 1..4
const Gc kKH[4] = {{-5.33980, -0.10008, 1.13098}, {-0.35351, 1.26970, 0.45400}, {-0.23789, 0.86036, 0.15354}, {-0.94158, 0.64552, 0.16817}};
const Gc kKV[4] = {{-3.80595, 0.56934, 0.81061}, {-3.44965, -0.22911, 0.51059}, {-0.39902, 0.73042, 0.11899}, {0.50167, 1.07319, 0.27195}};
const Gc kAH[5] = {{-0.14318, 1.82442, -0.55187}, {0.29591, 0.77564, 0.19822}, {0.32177, 0.63773, 0.13164}, {-5.37610, -0.96230, 1.47828},
                   {16.1721, -3.29980, 3.43990}};
const Gc kAV[5] = {{-0.07771, 2.33840, -0.76284}, {0.56727, 0.95545, 0.54039}, {-0.20238, 1.14520, 0.26809}, {-48.2991, 0.791669, 0.116226},
                   {48.5833, 0.791459, 0.116479}};
double gsum(const Gc* t, int n, double lf) {
    double s = 0;
    for (int j = 0; j < n; j++) { const double u = (lf - t[j].b) / t[j].c; s += t[j].a * std::exp(-u * u); }
    return s;
}
}

void rainCoefficients(double fGHz, double elevDeg, double tauDeg, double& k, double& alpha) {
    const double lf = std::log10(std::clamp(fGHz, 1.0, 1000.0));
    const double kH = std::pow(10.0, gsum(kKH, 4, lf) - 0.18961 * lf + 0.71147);   // eq. (2), Table 1
    const double kV = std::pow(10.0, gsum(kKV, 4, lf) - 0.16398 * lf + 0.63297);   // Table 2
    const double aH = gsum(kAH, 5, lf) + 0.67849 * lf - 1.95537;                    // eq. (3), Table 3
    const double aV = gsum(kAV, 5, lf) - 0.053739 * lf + 0.83433;                   // Table 4
    const double c = std::cos(elevDeg * kDeg), ct = c * c * std::cos(2 * tauDeg * kDeg);
    k = (kH + kV + (kH - kV) * ct) / 2;                                              // eq. (4)
    alpha = (kH * aH + kV * aV + (kH * aH - kV * aV) * ct) / (2 * k);               // eq. (5)
}

double rainGammaDbKm(double fGHz, double R, double elevDeg, double tauDeg) {
    if (R <= 0 || fGHz < 1.0) return 0;   // P.838 starts at 1 GHz; below it rain attenuation is negligible
    double k, a;
    rainCoefficients(fGHz, elevDeg, tauDeg, k, a);
    return k * std::pow(R, a);
}

double k50FromDeltaN(double dN) { return 157.0 / (157.0 - std::clamp(dN, 0.0, 150.0)); }

double rainHeightKm(double lat) {
    const double a = std::fabs(lat);
    return std::max(0.0, a <= 23 ? 5.0 : 5.0 - 0.075 * (a - 23));
}

double rainTerrestrialDb(double fGHz, double dKm, double R, double p, double tau) {
    if (R <= 0 || dKm <= 0 || fGHz < 1.0) return 0;
    double k, al;
    rainCoefficients(fGHz, 0, tau, k, al);
    const double g = k * std::pow(R, al);
    const double fr = std::max(fGHz, 6.0), dr = std::min(dKm, 30.0), Rr = std::max(R, 3.0);   // the limits of eq. (32)
    double r = 1.0 / (0.477 * std::pow(dr, 0.633) * std::pow(Rr, 0.073 * al) * std::pow(fr, 0.123) - 10.579 * (1 - std::exp(-0.024 * dr)));
    if (!(r > 0)) r = 2.5;
    const double A001 = g * dKm * r;                                                               // eq. (33)
    const double C0 = fGHz >= 10 ? 0.12 + 0.4 * std::log10(std::pow(fGHz / 10.0, 0.8)) : 0.12;
    const double C1 = std::pow(0.07, C0) * std::pow(0.12, 1 - C0);
    const double C2 = 0.855 * C0 + 0.546 * (1 - C0), C3 = 0.139 * C0 + 0.043 * (1 - C0);
    const double pp = std::clamp(p, 0.001, 1.0);
    return A001 * C1 * std::pow(pp, -(C2 + C3 * std::log10(pp)));                                // eq. (34)
}

double rainSlantDb(double f, double el, double lat, double hs, double R, double p, double tau, double hr) {
    if (R <= 0 || f < 1.0 || el <= 0) return 0;
    const double hR = hr >= 0 ? hr : rainHeightKm(lat);                                            // step 1
    if (hR - hs <= 0) return 0;
    const double th = el * kDeg;
    const double Re = 8500.0;                                                                       // effective earth radius, P.618
    double Ls = el >= 5 ? (hR - hs) / std::sin(th)                                                  // step 2, eq. (1) / (2)
                        : 2 * (hR - hs) / (std::sqrt(std::sin(th) * std::sin(th) + 2 * (hR - hs) / Re) + std::sin(th));
    const double LG = Ls * std::cos(th);                                                            // step 3
    const double g = rainGammaDbKm(f, R, el, tau);                                                  // step 5
    const double r001 = 1.0 / (1 + 0.78 * std::sqrt(LG * g / f) - 0.38 * (1 - std::exp(-2 * LG)));  // step 6
    const double zeta = std::atan2(hR - hs, LG * r001) / kDeg;                                      // step 7
    const double LR = zeta > el ? LG * r001 / std::cos(th) : (hR - hs) / std::sin(th);
    const double chi = std::fabs(lat) < 36 ? 36 - std::fabs(lat) : 0;
    const double v001 = 1.0 / (1 + std::sqrt(std::sin(th)) * (31 * (1 - std::exp(-(el / (1 + chi)))) * std::sqrt(LR * g) / (f * f) - 0.45));
    const double A001 = g * LR * v001;                                                              // steps 8, 9
    const double pp = std::clamp(p, 0.001, 5.0);                                                    // step 10
    double beta = 0;
    if (pp >= 1 || std::fabs(lat) >= 36) beta = 0;
    else if (el >= 25) beta = -0.005 * (std::fabs(lat) - 36);
    else beta = -0.005 * (std::fabs(lat) - 36) + 1.8 - 4.25 * std::sin(th);
    const double ex = -(0.655 + 0.033 * std::log(pp) - 0.045 * std::log(A001) - beta * (1 - pp) * std::sin(th));
    return A001 * std::pow(pp / 0.01, ex);
}

double scintillationDb(double f, double el, double D, double p, double nWet, double eta) {
    if (el <= 0) return 0;
    const double th = std::max(el, 5.0) * kDeg, s = std::sin(th);
    const double sref = 3.6e-3 + 1e-4 * nWet;                                                       // step 3
    const double L = 2 * 1000.0 / (std::sqrt(s * s + 2.35e-4) + s);                                // step 4, hL = 1000 m
    const double Deff = std::sqrt(eta) * std::max(D, 0.01);                                         // step 5
    const double x = 1.22 * Deff * Deff * f / L;                                                    // step 6
    const double g2 = 3.86 * std::pow(x * x + 1, 11.0 / 12.0) * std::sin(11.0 / 6.0 * std::atan(1 / x)) - 7.08 * std::pow(x, 5.0 / 6.0);
    const double gx = g2 > 0 ? std::sqrt(g2) : 0;
    const double sigma = sref * std::pow(f, 7.0 / 12.0) * gx / std::pow(s, 1.2);                   // step 7
    const double lp = std::log10(std::clamp(p, 0.01, 50.0));
    const double ap = -0.061 * lp * lp * lp + 0.072 * lp * lp - 1.71 * lp + 3.0;                    // step 8
    return ap * sigma;
}

double totalEarthSpaceDb(double gas, double rain, double cloud, double sc) { return gas + std::sqrt((rain + cloud) * (rain + cloud) + sc * sc); }

double invQ(double x) {
    if (x <= 0) return 8.0;
    if (x >= 1) return -8.0;
    if (x > 0.5) return -invQ(1 - x);
    const double T = std::sqrt(-2 * std::log(x));
    const double xi = ((0.010328 * T + 0.802853) * T + 2.515516698) / (((0.001308 * T + 0.189269) * T + 1.432788) * T + 1);
    return T - xi;
}

} // namespace dect2::prop::itu
