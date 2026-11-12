// HF sky-wave prediction after ITU-R P.533-14 with the ITU-R P.1239-4 ionosphere; see hf_p533.h. Written from the recommendation texts;
// equation numbers refer to P.533-14 unless another recommendation is named.
#include "dect2/prop/hf_p533.h"
#include "dect2/prop/prop.h"
#include <algorithm>
#include <cmath>

namespace dect2::prop::hf {
namespace {
#include "ccir_maps.inc"
#include "dgrf1960.inc"
#include "p1239_deciles.inc"
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double R0 = 6371.0;
constexpr double kLz = 8.72;   // P.533-14 section 5.2.2
constexpr double kLy = -0.14;  // section 5.3.3

double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

// great-circle point at distance x (km) from (la1, lo1) toward (la2, lo2)
struct GC {
    double la1, lo1, la2, lo2, D, brg;
    GC(double a, double b, double c, double d) : la1(a), lo1(b), la2(c), lo2(d) {
        D = greatCircleKm(a, b, c, d);
        brg = bearingDeg(a, b, c, d);
    }
    void at(double x, double& la, double& lo) const { destination(la1, lo1, brg, clampd(x, 0, D), la, lo); }
};

// Table 1 of P.1239: geographic functions G_k for the per-longitude-order counts q (foF2: 12,12,9,5,2,1,1,1,1; M(3000)F2: 7,8,6,3,2,1,1)
const int kQf[9] = {12, 12, 9, 5, 2, 1, 1, 1, 1};
const int kQm[7] = {7, 8, 6, 3, 2, 1, 1};
void geoFunctions(double lat, double lon, double modip, const int* q, int nq, double* G) {
    const double sx = std::sin(modip * kDeg), cl = std::cos(lat * kDeg);
    int k = 0;
    double s = 1;
    for (int i = 0; i < q[0]; i++) { G[k++] = s; s *= sx; }
    double clj = 1;
    for (int j = 1; j < nq; j++) {
        clj *= cl;
        const double cj = std::cos(j * lon * kDeg), sj = std::sin(j * lon * kDeg);
        s = clj;
        for (int i = 0; i < q[j]; i++) { G[k++] = s * cj; G[k++] = s * sj; s *= sx; }
    }
}
// a_s(lambda, theta) = sum_k U_{s,k} G_k for both solar levels (P.1239 eqs. (2), (3)), then the time series at T
void mapCoefs(const float* U, int ns, int nk, const double* G, double* a0, double* a100) {
    for (int s = 0; s < ns; s++) {
        double x0 = 0, x1 = 0;
        for (int k = 0; k < nk; k++) { x0 += U[s + ns * k] * G[k]; x1 += U[s + ns * (k + nk)] * G[k]; }
        a0[s] = x0; a100[s] = x1;
    }
}
double timeSeries(const double* a, int ns, double utc) {
    const double T = (15.0 * utc - 180.0) * kDeg;   // universal time as an angle
    double v = a[0];
    for (int j = 1; 2 * j < ns; j++) v += a[2 * j - 1] * std::sin(j * T) + a[2 * j] * std::cos(j * T);
    return v;
}
double ccir(bool fo, int month, double lat, double lon, double utc, double r12) {
    const int m = ((month - 1) % 12 + 12) % 12;
    const float* U = fo ? kCcir[m] : kCcir[m] + 1976;
    const int ns = fo ? 13 : 9, nk = fo ? 76 : 49;
    double G[76], a0[13], a1[13];
    geoFunctions(lat, lon, modipDeg(lat, lon), fo ? kQf : kQm, fo ? 9 : 7, G);
    mapCoefs(U, ns, nk, G, a0, a1);
    const double v0 = timeSeries(a0, ns, utc), v1 = timeSeries(a1, ns, utc);
    const double R = clampd(r12, 0, 160);
    return v0 + (v1 - v0) * R / 100.0;
}

int seasonOf(int month, double lat) {   // 0 winter, 1 equinox, 2 summer (P.533 Lh convention: Dec-Feb winter in the north)
    int s = (month == 12 || month <= 2) ? 0 : (month >= 6 && month <= 8) ? 2 : 1;
    if (lat < 0 && s != 1) s = 2 - s;
    return s;
}

// P.533 section 5.1, eq. (13): elevation angle of a hop d (km) reflected at hr (km)
double elevationDeg(double d, double hr) {
    const double t = d / (2 * R0);
    return std::atan((std::cos(t) - R0 / (R0 + hr)) / std::sin(t)) / kDeg;
}
// eq. (19): virtual slant range of n hops
double slantKm(int n, double d, double elevDeg) {
    const double t = d / (2 * R0);
    return 2 * R0 * n * std::sin(t) / std::cos(elevDeg * kDeg + t);
}
double incidenceSec(double elevDeg, double hKm) {   // sec of the incidence angle at height h of a ray leaving the ground at elev
    const double s = R0 * std::cos(elevDeg * kDeg) / (R0 + hKm);
    return 1.0 / std::sqrt(std::max(1 - s * s, 1e-6));
}
// ground distance from the take-off point to where the ray crosses height h
double groundToHeightKm(double elevDeg, double hKm) {
    const double e = elevDeg * kDeg;
    const double phi = kPi / 2 - e - std::asin(clampd(R0 * std::cos(e) / (R0 + hKm), -1, 1));
    return R0 * phi;
}

// P.1240-2 eqs. (3)-(6): F2 MUF of a hop d at a control point
struct F2Cp { double foF2, m3000, foE, fH; };
double bFactor(const F2Cp& c) {
    const double x = std::max(c.foF2 / std::max(c.foE, 0.1), 2.0);
    return c.m3000 - 0.124 + (c.m3000 * c.m3000 - 4) * (0.0215 + 0.005 * std::sin(7.854 / x - 1.9635));   // eq. (6)
}
double dmaxKm(const F2Cp& c, bool limit) {
    const double x = std::max(c.foF2 / std::max(c.foE, 0.1), 2.0), x2 = x * x;
    const double v = 4780 + (12610 + 2140 / x2 - 49720 / (x2 * x2) + 688900 / (x2 * x2 * x2)) * (1 / bFactor(c) - 0.303);   // eq. (5)
    return limit ? std::min(v, 4000.0) : v;
}
double cdOf(double d, double dmax) {
    const double Z = 1 - 2 * d / dmax;
    return 0.74 - 0.591 * Z - 0.424 * Z * Z - 0.090 * Z * Z * Z + 0.088 * std::pow(Z, 4) + 0.181 * std::pow(Z, 5) + 0.096 * std::pow(Z, 6);   // eq. (4)
}
double f2Muf(const F2Cp& c, double d, bool limit) {
    const double dm = dmaxKm(c, limit), dd = std::min(d, dm);
    const double B = bFactor(c);
    return (1 + cdOf(dd, dm) / cdOf(3000, dm) * (B - 1)) * c.foF2 + c.fH / 2 * (1 - dd / dm);    // eq. (3)
}

// section 5.1: F2 mirror reflection height for a hop d at frequency f
double f2MirrorKm(const F2Cp& c, double r12, double f, double d) {
    const double x = c.foF2 / std::max(c.foE, 0.1), y = std::max(x, 1.8);
    const double dM = 0.18 / (y - 1.4) + 0.096 * (r12 - 25) / 150;
    const double H = 1490 / (c.m3000 + dM) - 316;
    const double xr = f / c.foF2;
    double h;
    if (x > 3.33 && xr >= 1) {                                                   // eq. (14)
        const double E1 = -0.09707 * xr * xr * xr + 0.6870 * xr * xr - 0.7506 * xr + 0.6;
        const double F1 = xr <= 1.71 ? -1.862 * std::pow(xr, 4) + 12.95 * xr * xr * xr - 32.03 * xr * xr + 33.50 * xr - 10.91 : 1.21 + 0.2 * xr;
        const double G = xr <= 3.7 ? -2.102 * std::pow(xr, 4) + 19.50 * xr * xr * xr - 63.15 * xr * xr + 90.47 * xr - 44.73 : 19.25;
        const double A1 = 140 + (H - 47) * E1, B1 = 150 + (H - 17) * F1 - A1;
        const double ds = 160 + (H + 43) * G, a = (d - ds) / (H + 140);
        h = (B1 >= 0 && a >= 0) ? A1 + B1 * std::pow(2.4, -a) : A1 + B1;
    } else if (x > 3.33) {                                                       // eq. (15)
        const double Z = std::max(xr, 0.1);
        const double E2 = 0.1906 * Z * Z + 0.00583 * Z + 0.1936, F2 = 0.645 * Z * Z + 0.883 * Z + 0.162;
        const double A2 = 151 + (H - 47) * E2, B2 = 141 + (H - 24) * F2 - A2;
        const double df = std::min(0.115 * d / (Z * (H + 140)), 0.65);
        const double b = -7.535 * std::pow(df, 4) + 15.75 * df * df * df - 8.834 * df * df - 0.378 * df + 1;
        h = B2 >= 0 ? A2 + B2 * b : A2 + B2;
    } else {                                                                     // eq. (16)
        const double J = -0.7126 * y * y * y + 5.863 * y * y - 16.13 * y + 16.07;
        const double U = 8e-5 * (H - 80) * (1 + 11 * std::pow(y, -2.2)) + 1.2e-3 * H * std::pow(y, -3.6);
        h = 115 + H * J + U * d;
    }
    return clampd(h, 120, 800);
}

// P.533 Table 2: auroral and other losses Lh [range <= 2500 km, > 2500][season][geomagnetic latitude band 77.5, 72.5 .. 42.5][time block]
const float kLh[2][3][8][8] = {
 {{{2.0f,6.6f,6.2f,1.5f,0.5f,1.4f,1.5f,1.0f},{3.4f,8.3f,8.6f,0.9f,0.5f,2.5f,3.0f,3.0f},{6.2f,15.6f,12.8f,2.3f,1.5f,4.6f,7.0f,5.0f},{7.0f,16.0f,14.0f,3.6f,2.0f,6.8f,9.8f,6.6f},
   {2.0f,4.5f,6.6f,1.4f,0.8f,2.7f,3.0f,2.0f},{1.3f,1.0f,3.2f,0.3f,0.4f,1.8f,2.3f,0.9f},{0.9f,0.6f,2.2f,0.2f,0.2f,1.2f,1.5f,0.6f},{0.4f,0.3f,1.1f,0.1f,0.1f,0.6f,0.7f,0.3f}},
  {{1.4f,2.5f,7.4f,3.8f,1.0f,2.4f,2.4f,3.3f},{3.3f,11.0f,11.6f,5.1f,2.6f,4.0f,6.0f,7.0f},{6.5f,12.0f,21.4f,8.5f,4.8f,6.0f,10.0f,13.7f},{6.7f,11.2f,17.0f,9.0f,7.2f,9.0f,10.9f,15.0f},
   {2.4f,4.4f,7.5f,5.0f,2.6f,4.8f,5.5f,6.1f},{1.7f,2.0f,5.0f,3.0f,2.2f,4.0f,3.0f,4.0f},{1.1f,1.3f,3.3f,2.0f,1.4f,2.6f,2.0f,2.6f},{0.5f,0.6f,1.6f,1.0f,0.7f,1.3f,1.0f,1.3f}},
  {{2.2f,2.7f,1.2f,2.3f,2.2f,3.8f,4.2f,3.8f},{2.4f,3.0f,2.8f,3.0f,2.7f,4.2f,4.8f,4.5f},{4.9f,4.2f,6.2f,4.5f,3.8f,5.4f,7.7f,7.2f},{6.5f,4.8f,9.0f,6.0f,4.8f,9.1f,9.5f,8.9f},
   {3.2f,2.7f,4.0f,3.0f,3.0f,6.5f,6.7f,5.0f},{2.5f,1.8f,2.4f,2.3f,2.6f,5.0f,4.6f,4.0f},{1.6f,1.2f,1.6f,1.5f,1.7f,3.3f,3.1f,2.6f},{0.8f,0.6f,0.8f,0.7f,0.8f,1.6f,1.5f,1.3f}}},
 {{{1.5f,2.7f,2.5f,0.8f,0.0f,0.9f,0.8f,1.6f},{2.5f,4.5f,4.3f,0.8f,0.3f,1.6f,2.0f,4.8f},{5.5f,5.0f,7.0f,1.9f,0.5f,3.0f,4.5f,9.6f},{5.3f,7.0f,5.9f,2.0f,0.7f,4.0f,4.5f,10.0f},
   {1.6f,2.4f,2.7f,0.6f,0.4f,1.7f,1.8f,3.5f},{0.9f,1.0f,1.3f,0.1f,0.1f,1.0f,1.5f,1.4f},{0.6f,0.6f,0.8f,0.1f,0.1f,0.6f,1.0f,0.5f},{0.3f,0.3f,0.4f,0.0f,0.0f,0.3f,0.5f,0.4f}},
  {{1.0f,1.2f,2.7f,3.0f,0.6f,2.0f,2.3f,1.6f},{1.8f,2.9f,4.1f,5.7f,1.5f,3.2f,5.6f,3.6f},{3.7f,5.6f,7.7f,8.1f,3.5f,5.0f,9.5f,7.3f},{3.9f,5.2f,7.6f,9.0f,5.0f,7.5f,10.0f,7.9f},
   {1.4f,2.0f,3.2f,3.8f,1.8f,4.0f,5.4f,3.4f},{0.9f,0.9f,1.8f,2.0f,1.3f,3.1f,2.7f,2.0f},{0.6f,0.6f,1.2f,1.3f,0.8f,2.0f,1.8f,1.3f},{0.3f,0.3f,0.6f,0.6f,0.4f,1.0f,0.9f,0.6f}},
  {{1.9f,3.8f,2.2f,1.1f,2.1f,1.2f,2.3f,2.4f},{1.9f,4.6f,2.9f,1.3f,2.2f,1.3f,2.8f,2.7f},{4.4f,6.3f,5.9f,1.9f,3.3f,1.7f,4.4f,4.5f},{5.5f,8.5f,7.6f,2.6f,4.2f,3.2f,5.5f,5.7f},
   {2.8f,3.8f,3.7f,1.4f,2.7f,1.6f,4.5f,3.2f},{2.2f,2.4f,2.2f,1.0f,2.2f,1.2f,4.4f,2.5f},{1.4f,1.6f,1.4f,0.6f,1.4f,0.8f,2.9f,1.6f},{0.7f,0.8f,0.7f,0.3f,0.7f,0.4f,1.4f,0.8f}}}};
double lhAt(double lat, double lon, int month, double utc, bool longRange) {
    const double gn = std::fabs(geomagneticLatDeg(lat, lon));
    if (gn < 42.5) return 0;
    const int band = gn >= 77.5 ? 0 : 7 - std::min(7, (int)((gn - 42.5) / 5.0));
    double lt = std::fmod(utc + lon / 15.0 + 48.0, 24.0);
    const int blk = (int)std::floor(std::fmod(lt - 1.0 + 24.0, 24.0) / 3.0) % 8;   // 01-04, 04-07, ..., 22-01
    return kLh[longRange ? 1 : 0][seasonOf(month, lat)][band][blk];
}

// P.842-5 Table 2: day-to-day decile deviations of the signal against f / basic MUF
void signalDeciles(double ratio, bool highLat, double& ld, double& ud) {
    static const double r[10] = {0.8, 1.0, 1.2, 1.4, 1.6, 1.8, 2.0, 3.0, 4.0, 5.0};
    static const double L0[10] = {8, 12, 13, 10, 8, 8, 8, 7, 6, 5}, U0[10] = {6, 8, 12, 13, 12, 9, 9, 8, 7, 7};
    static const double L1[10] = {11, 16, 17, 13, 11, 11, 11, 9, 8, 7}, U1[10] = {9, 11, 12, 13, 12, 9, 9, 8, 7, 7};
    const double* L = highLat ? L1 : L0; const double* U = highLat ? U1 : U0;
    if (ratio <= r[0]) { ld = L[0]; ud = U[0]; return; }
    if (ratio >= r[9]) { ld = L[9]; ud = U[9]; return; }
    int i = 0;
    while (ratio > r[i + 1]) i++;
    const double t = (ratio - r[i]) / (r[i + 1] - r[i]);
    ld = L[i] + t * (L[i + 1] - L[i]); ud = U[i] + t * (U[i + 1] - U[i]);
}

double absorptionHopDb(double f, double elev, double la1, double lo1, double la2, double lo2, int month, double utc, double r12) {
    // CCIR Report 252 form at the two 90 km penetration points: 677.2 sec(i) I / ((f + fL)^1.98 + 10.2), I = (1 + 0.0037 R) F(chi)
    double sum = 0;
    const double sec = incidenceSec(elev, 100);
    const double pts[2][2] = {{la1, lo1}, {la2, lo2}};
    for (const auto& q : pts) {
        double dip, F;
        magField(q[0], q[1], 100, dip, F);
        const double fL = std::fabs(2.8 * F * std::sin(dip * kDeg));
        const double chi = std::min(solarZenithDeg(q[0], q[1], month, utc), 102.0);
        const double Fc = std::max(std::pow(std::max(std::cos(0.881 * chi * kDeg), 0.0), 1.3), 0.02);   // P.533 eq. (21) floor
        sum += 677.2 * sec * (1 + 0.0037 * r12) * Fc / (std::pow(f + fL, 1.98) + 10.2);
    }
    return sum / 2;
}

struct Cp { double lat, lon; F2Cp c; };
Cp cpAt(const GC& g, double x, const Path& p, double r12, double sfi12) {
    Cp o;
    g.at(x, o.lat, o.lon);
    o.c.foF2 = std::max(foF2(p.month, o.lat, o.lon, p.utcHour, r12), 0.5);
    o.c.m3000 = clampd(m3000F2(p.month, o.lat, o.lon, p.utcHour, r12), 2.0, 4.5);
    o.c.foE = foE(p.month, o.lat, o.lon, p.utcHour, sfi12);
    o.c.fH = gyroMHz(o.lat, o.lon, 300);
    return o;
}

// section 5.2: ray-path modes and the root-sum-square field (paths up to 9000 km)
double shortPathField(const Path& p, const GC& g, double r12, double sfi12, Prediction& o) {
    const double D = std::max(g.D, 1.0), f = p.fMHz;
    const Cp M = cpAt(g, D / 2, p, r12, sfi12);
    const Cp T1 = cpAt(g, std::min(1000.0, D / 2), p, r12, sfi12), R1 = cpAt(g, std::max(D - 1000.0, D / 2), p, r12, sfi12);
    o.foF2Mid = M.c.foF2; o.foEMid = M.c.foE; o.m3000Mid = M.c.m3000;
    const double ptDbkW = p.txPowerDbw - 30;
    // E-layer screening foE (section 4) and E modes (section 3.3)
    const double foEscr = D <= 2000 ? M.c.foE : std::max(T1.c.foE, R1.c.foE);
    const double foEmuf = D <= 2000 ? M.c.foE : std::min(T1.c.foE, R1.c.foE);
    // Lh: mean over the absorption control points of Table 1d
    double lh = 0; int nlh = 0;
    for (double x : {std::min(1000.0, D / 2), D / 2, std::max(D - 1000.0, D / 2)}) { double la, lo; g.at(x, la, lo); lh += lhAt(la, lo, p.month, p.utcHour, D > 2500); nlh++; }
    lh /= nlh;
    double sumE = 0;
    double bmufE = 0, bmufF = 0;
    o.modes.clear();
    auto addMode = [&](Mode& m) {
        const double sl = slantKm(m.hops, m.hopKm, m.elevDeg);
        // absorption: every hop's two 90 km penetration points (300 km reflection for the geometry, section 5.2.2)
        const double e300 = elevationDeg(m.hopKm, 300);
        const double gx = groundToHeightKm(std::max(e300, 0.1), 90);
        double Li = 0;
        for (int h = 0; h < m.hops; h++) {
            double la1, lo1, la2, lo2;
            g.at(h * m.hopKm + gx, la1, lo1); g.at((h + 1) * m.hopKm - gx, la2, lo2);
            Li += absorptionHopDb(f, m.elevDeg, la1, lo1, la2, lo2, p.month, p.utcHour, r12);
        }
        m.absorptionDb = Li;
        const double fb = m.basicMufMHz;
        if (f > fb) m.aboveMufDb = m.layer == 'E' ? std::min(130 * (f / fb - 1) * (f / fb - 1), 81.0) : std::min(36 * std::sqrt(f / fb - 1), 62.0);   // (25), (26)
        m.groundDb = 2.0 * (m.hops - 1);                                                                       // eq. (27)
        m.auroralDb = lh;
        m.lossDb = 32.45 + 20 * std::log10(f) + 20 * std::log10(sl) + Li + m.aboveMufDb + m.groundDb + m.auroralDb + kLz;   // eq. (18)
        m.fieldDbuv = 136.6 + ptDbkW + p.txGainDbi + 20 * std::log10(f) - m.lossDb;                          // eq. (17)
        m.delayMs = sl / 299792.458 * 1e3;                                                                     // eq. (47)
        if (!m.screened) sumE += std::pow(10.0, m.fieldDbuv / 10);
        o.modes.push_back(m);
    };
    if (D <= 4000) {
        int n0 = (int)std::ceil(D / 2000.0);
        for (int n = n0; n < n0 + 3; n++) {
            Mode m; m.layer = 'E'; m.hops = n; m.hopKm = D / n; m.mirrorKm = 110;
            m.elevDeg = elevationDeg(m.hopKm, 110);
            if (m.elevDeg < 0) continue;
            m.basicMufMHz = foEmuf * incidenceSec(m.elevDeg, 110);                                              // eq. (1)
            if (n == n0) bmufE = m.basicMufMHz;
            addMode(m);
        }
    }
    // F2 modes (section 3.5): the lowest order with a hop up to dmax, and five more
    const double dmax = dmaxKm(M.c, true);
    const int n0 = std::max(1, (int)std::ceil(D / dmax));
    // for paths longer than dmax: F2(dmax)MUF at the two control points d0 / 2 from each end
    const double d0 = D / n0;
    const Cp Ta = cpAt(g, d0 / 2, p, r12, sfi12), Ra = cpAt(g, D - d0 / 2, p, r12, sfi12);
    for (int n = n0; n < n0 + 6; n++) {
        Mode m; m.layer = 'F'; m.hops = n; m.hopKm = D / n;
        if (D <= dmax) {
            m.basicMufMHz = f2Muf(M.c, m.hopKm, true);
            m.mirrorKm = f2MirrorKm(M.c, r12, f, m.hopKm);
        } else {
            double best = 1e9, hr = 0;
            for (const Cp* c : {&Ta, &Ra}) {
                const double base = f2Muf(c->c, dmaxKm(c->c, true), true);                                      // F2(dmax)MUF
                const double ratio = f2Muf(c->c, m.hopKm, false) / std::max(f2Muf(c->c, d0, false), 0.1);     // Mn / Mn0, eq. (8)
                best = std::min(best, base * (n == n0 ? 1.0 : ratio));
                hr += f2MirrorKm(c->c, r12, f, m.hopKm) / 2;
            }
            m.basicMufMHz = best;
            m.mirrorKm = (hr * 2 + f2MirrorKm(M.c, r12, f, m.hopKm)) / 3;   // mean of the Table 1c control points
        }
        m.elevDeg = elevationDeg(m.hopKm, m.mirrorKm);
        if (n == n0) bmufF = m.basicMufMHz;   // the path basic MUF is the lowest-order mode's, whatever its take-off angle
        if (m.elevDeg < 0) continue;          // below the horizon for this mirror height: no ray (the next order takes over)
        if (D <= 4000) m.screened = 1.05 * foEscr * incidenceSec(m.elevDeg, 110) >= f;                         // eqs. (11), (12)
        addMode(m);
    }
    o.basicMufMHz = std::max(bmufE, bmufF);
    // operational MUF (P.533 3.7, P.1240-2 Table 1) and its deciles (P.1239 Tables 2, 3)
    const double chiM = solarZenithDeg(M.lat, M.lon, p.month, p.utcHour);
    const int season = seasonOf(p.month, M.lat);
    const bool hiPower = p.txPowerDbw + p.txGainDbi > 30;
    const double rop = (season == 2 ? (chiM < 90 ? 1.10 : 1.20) : season == 1 ? (chiM < 90 ? 1.15 : 1.25) : (chiM < 90 ? 1.20 : 1.30)) + (hiPower ? 0.05 : 0);
    const double opF = bmufF * rop;
    const double lt = std::fmod(p.utcHour + M.lon / 15.0 + 48, 24.0);
    const double dl = foF2DecileFactor(false, p.month, M.lat, lt, r12), du = foF2DecileFactor(true, p.month, M.lat, lt, r12);
    if (opF >= bmufE) { o.opMufMHz = opF; o.owfMHz = opF * dl; o.hpfMHz = opF * du; }
    else { o.opMufMHz = bmufE; o.owfMHz = bmufE * 0.95; o.hpfMHz = bmufE * 1.05; }
    int best = -1;
    for (size_t i = 0; i < o.modes.size(); i++)
        if (!o.modes[i].screened && (best < 0 || o.modes[i].fieldDbuv > o.modes[best].fieldDbuv)) best = (int)i;
    o.bestMode = best;
    return sumE > 0 ? 10 * std::log10(sumE) : -999;                                                            // eq. (28)
}

// section 5.3: composite long-path method
double longPathField(const Path& p, const GC& g, double r12, double sfi12, Prediction& o) {
    const double D = g.D, f = p.fMHz;
    int nM = (int)std::ceil(D / 4000.0);
    while (elevationDeg(D / nM, 300) < 3.0 && nM < 20) nM++;
    const double dM = D / nM;
    // fD polynomial, eq. (30)
    const double C[7] = {29.1996868566837e-6, 87.4376851991085e-9, 22.0776941764705e-12, 102.342990689362e-15, -92.4986988833091e-18,
                         25.8520201885984e-21, -2.40074637494790e-24};
    double fD = C[6];
    for (int i = 5; i >= 0; i--) fD = fD * dM + C[i];
    fD *= dM;
    // W, X, Y interpolated by the azimuth at the path centre (Table 3)
    double mla, mlo; g.at(D / 2, mla, mlo);
    double la2, lo2; g.at(std::min(D / 2 + 100, D), la2, lo2);
    double az = std::fmod(bearingDeg(mla, mlo, la2, lo2) + 360.0, 180.0);
    const double fromNS = std::min(az, 180 - az) / 90.0;   // 0 north-south .. 1 east-west
    const double W = 0.2 + (0.1 - 0.2) * fromNS, X = 0.2 + (1.2 - 0.2) * fromNS, Y = 0.4 + (0.6 - 0.4) * fromNS;
    double fM = 1e9, fBMpath = 1e9, fHsum = 0;
    const int hNow = ((int)std::floor(p.utcHour) % 24 + 24) % 24;
    for (double x : {dM / 2, D - dM / 2}) {
        double la, lo; g.at(x, la, lo);
        double fo[24], mm[24];
        foF2Day(p.month, la, lo, r12, fo, mm);
        const double fH = gyroMHz(la, lo, 300);
        fHsum += fH;
        double fbm[24], fmin = 1e9;
        for (int h = 0; h < 24; h++) {
            const double f4 = 1.1 * fo[h] * clampd(mm[h], 2.0, 4.5), fz = fo[h] + fH / 2;
            fbm[h] = fz + (f4 - fz) * fD;                                                                          // eq. (29)
            fmin = std::min(fmin, fbm[h]);
        }
        const int hNoon = ((int)std::lround(12 - lo / 15.0) % 24 + 24) % 24;
        const double fb = fbm[hNow], fn = fbm[hNoon];
        const double K = 1.2 + W * fb / fn + X * (std::cbrt(fn / fb) - 1) + Y * (fmin / fn) * (fmin / fn);       // eq. (32)
        fM = std::min(fM, K * fb);                                                                                // eq. (31)
        fBMpath = std::min(fBMpath, fb);
    }
    const double fH = fHsum / 2;
    // fL over 24 hours (section 5.3.2)
    const int nL = (int)std::ceil(D / 3000.0);
    const double dL = D / nL, eL = std::max(elevationDeg(dL, 300), 0.1);
    const double gx = groundToHeightKm(eL, 90), cosI90 = 1 / incidenceSec(eL, 90), pL = slantKm(nL, dL, eL);
    const double alat = std::fabs(mla);
    static const double AwN[12] = {0.30, 0.15, 0.03, 0, 0, 0, 0, 0, 0.01, 0.03, 0.15, 0.30};
    static const double AwS[12] = {0, 0, 0, 0.03, 0.15, 0.30, 0.30, 0.15, 0.03, 0, 0, 0};
    const double aw60 = (mla >= 0 ? AwN : AwS)[(p.month - 1 + 12) % 12];
    const double Aw = alat <= 30 ? 0 : alat <= 60 ? aw60 * (alat - 30) / 30 : aw60 * (90 - alat) / 30;
    double fl[24];
    const double fLN = std::sqrt(D / 3000.0);                                                                    // eq. (36)
    for (int h = 0; h < 24; h++) {
        double s = 0;
        for (int k = 0; k < nL; k++)
            for (double x : {k * dL + gx, (k + 1) * dL - gx}) {
                double la, lo; g.at(x, la, lo);
                const double chi = solarZenithDeg(la, lo, p.month, h + 0.5);
                if (chi < 90) s += std::sqrt(std::cos(chi * kDeg));
            }
        const double v = (5.3 * std::sqrt((1 + 0.009 * r12) * s / (cosI90 * std::log(9.5e6 / pL))) - fH) * (Aw + 1);   // eq. (33)
        fl[h] = std::max(v, fLN);
    }
    const double e23 = std::exp(-0.23);
    for (int h = 0; h < 24; h++) {   // the day-to-night decay after sunset, eqs. (37), (38)
        const int hp = (h + 23) % 24;
        if (fl[h] < 2 * fLN && fl[hp] > 2 * fLN) {
            const double dt = (2 * fLN - fl[h]) / (fl[hp] - fl[h]);
            double v = e23 * fl[hp] * (dt * (1 - e23) + e23);
            fl[h] = std::max(fl[h], v);
            for (int k = 1; k <= 3; k++) { v *= e23; const int hk = (h + k) % 24; fl[hk] = std::max(fl[hk], v); }
        }
    }
    const double fL = fl[hNow];
    o.lufLongMHz = fL;
    // eq. (39): the composite field strength
    const double eM = elevationDeg(dM, 300);
    const double pp = slantKm(nM, dM, std::max(eM, 0.1));
    const double E0 = 139.6 - 20 * std::log10(pp);                                                               // eq. (40)
    double Gap = 10 * std::log10(D / (R0 * std::fabs(std::sin(D / R0))));                                       // eq. (41)
    if (!(Gap < 15)) Gap = 15;
    const double a = (fM + fH) * (fM + fH), b = (fL + fH) * (fL + fH), c = (f + fH) * (f + fH);
    const double brk = 1 - a / (a + b) * (b / c + c / a);
    if (o.basicMufMHz <= 0) { o.basicMufMHz = fBMpath; o.opMufMHz = fM; o.owfMHz = fM * 0.85; o.hpfMHz = fM * 1.15; }
    return E0 * brk - 30.0 + (p.txPowerDbw - 30) + p.txGainDbi + Gap - kLy;
}
}

// ---- field model and maps ----
void magField(double lat, double lon, double hKm, double& dipDeg, double& totalGauss) {
    const double a = 6371.2, r = a + hKm;
    const double th = (90 - clampd(lat, -89.999, 89.999)) * kDeg, ph = lon * kDeg;
    const double ct = std::cos(th), st = std::sin(th);
    constexpr int N = 6;
    double P[N + 1][N + 1] = {{0}}, dP[N + 1][N + 1] = {{0}};
    P[0][0] = 1;
    for (int n = 1; n <= N; n++)
        for (int m = 0; m <= n; m++) {
            if (n == m) {
                const double k = n > 1 ? std::sqrt(1 - 1.0 / (2 * n)) : 1.0;
                P[n][n] = k * st * P[n - 1][n - 1];
                dP[n][n] = k * (st * dP[n - 1][n - 1] + ct * P[n - 1][n - 1]);
            } else {
                const double k1 = (2 * n - 1) / std::sqrt((double)(n * n - m * m));
                const double k2 = n > 1 ? std::sqrt(((n - 1) * (n - 1) - m * m) / (double)(n * n - m * m)) : 0.0;
                P[n][m] = k1 * ct * P[n - 1][m] - (n >= 2 ? k2 * P[n - 2][m] : 0);
                dP[n][m] = k1 * (ct * dP[n - 1][m] - st * P[n - 1][m]) - (n >= 2 ? k2 * dP[n - 2][m] : 0);
            }
        }
    double g[N + 1][N + 1] = {{0}}, h[N + 1][N + 1] = {{0}};
    for (const GaussCoef& c : kDgrf1960) (c.gh == 'g' ? g : h)[c.n][c.m] = c.v;
    double X = 0, Y = 0, Z = 0;
    for (int n = 1; n <= N; n++) {
        const double fr = std::pow(a / r, n + 2);
        for (int m = 0; m <= n; m++) {
            const double cm = std::cos(m * ph), sm = std::sin(m * ph);
            X += fr * (g[n][m] * cm + h[n][m] * sm) * dP[n][m];
            Y += fr * m * (g[n][m] * sm - h[n][m] * cm) * P[n][m] / st;
            Z -= fr * (n + 1) * (g[n][m] * cm + h[n][m] * sm) * P[n][m];
        }
    }
    dipDeg = std::atan2(Z, std::hypot(X, Y)) / kDeg;
    totalGauss = std::sqrt(X * X + Y * Y + Z * Z) * 1e-5;   // nT -> gauss
}
double modipDeg(double lat, double lon) {
    double I, F;
    magField(lat, lon, 300, I, F);
    return std::atan(I * kDeg / std::sqrt(std::max(std::cos(lat * kDeg), 1e-6))) / kDeg;   // P.1239 eq. (4)
}
double gyroMHz(double lat, double lon, double hKm) { double I, F; magField(lat, lon, hKm, I, F); return 2.8 * F; }   // P.1239 eq. (11)
double foF2(int month, double lat, double lon, double utc, double r12) { return ccir(true, month, lat, lon, utc, r12); }
double m3000F2(int month, double lat, double lon, double utc, double r12) { return ccir(false, month, lat, lon, utc, r12); }
void foF2Day(int month, double lat, double lon, double r12, double fo[24], double mm[24]) {
    const int m = ((month - 1) % 12 + 12) % 12;
    const double X = modipDeg(lat, lon), R = clampd(r12, 0, 160) / 100.0;
    double G[76], a0[13], a1[13], b0[9], b1[9];
    geoFunctions(lat, lon, X, kQf, 9, G);
    mapCoefs(kCcir[m], 13, 76, G, a0, a1);
    geoFunctions(lat, lon, X, kQm, 7, G);
    mapCoefs(kCcir[m] + 1976, 9, 49, G, b0, b1);
    for (int h = 0; h < 24; h++) {
        const double f0 = timeSeries(a0, 13, h), f1 = timeSeries(a1, 13, h), m0 = timeSeries(b0, 9, h), m1 = timeSeries(b1, 9, h);
        fo[h] = std::max(f0 + (f1 - f0) * R, 0.5);
        mm[h] = m0 + (m1 - m0) * R;
    }
}

double subsolarLatDeg(int month) {
    static const double sx[12] = {-21.2, -12.7, -2.2, 9.7, 18.8, 23.3, 21.6, 14.1, 3.1, -8.4, -18.4, -23.3};
    return sx[((month - 1) % 12 + 12) % 12];
}
double solarZenithDeg(double lat, double lon, int month, double utc) {
    const double dl = subsolarLatDeg(month) * kDeg, ph = lat * kDeg;
    const double eta = (utc / 12.0 - 1) * kPi + lon * kDeg;                                    // eq. (35)
    const double c = std::sin(ph) * std::sin(dl) + std::cos(ph) * std::cos(dl) * std::cos(eta);   // eq. (34)
    return std::acos(clampd(c, -1, 1)) / kDeg;
}
double foE(int month, double lat, double lon, double utc, double phi) {
    // P.1239-4 section 4, eqs. (12)-(18)
    const double A = 1 + 0.0094 * (phi - 66);
    const double alat = std::fabs(lat), cl = std::cos(lat * kDeg);
    double N = std::fabs(lat - subsolarLatDeg(month));
    if (N >= 80) N = 80;
    const double mexp = alat <= 32 ? -1.93 + 1.92 * cl : 0.11 - 0.49 * cl;
    const double B = std::pow(std::cos(N * kDeg), mexp);
    const double Cc = alat <= 32 ? 23 + 116 * cl : 92 + 35 * cl;
    const double pexp = alat <= 12 ? 1.31 : 1.20;
    const double chi = solarZenithDeg(lat, lon, month, utc);
    double D;
    if (chi <= 73) D = std::pow(std::cos(chi * kDeg), pexp);
    else if (chi < 90) { const double dc = 6.27e-13 * std::pow(chi - 50, 8); D = std::pow(std::cos((chi - dc) * kDeg), pexp); }
    else {
        double hs = -1;   // hours since sunset
        for (double t = 0.25; t <= 24; t += 0.25)
            if (solarZenithDeg(lat, lon, month, std::fmod(utc - t + 48, 24.0)) < 90) { hs = t; break; }
        const double d2 = std::pow(0.072, pexp) * std::exp(25.2 - 0.28 * chi);
        D = hs < 0 ? d2 : std::max(std::pow(0.072, pexp) * std::exp(-1.4 * hs), d2);
    }
    const double f4 = std::max(A * B * Cc * D, 0.004 * std::pow(1 + 0.021 * phi, 2));
    return std::pow(f4, 0.25);
}
double foF2DecileFactor(bool upper, int month, double lat, double localHour, double r12) {
    const int s = seasonOf(month, lat);
    const int ri = r12 < 50 ? 0 : r12 <= 100 ? 1 : 2;
    const double a = clampd(std::fabs(lat), 0, 90);
    const int i0 = std::min(18, (int)((90 - a) / 5)), i1 = std::min(18, i0 + 1);
    const double t = clampd((90 - a) / 5 - i0, 0, 1);
    const int h = ((int)std::lround(localHour) % 24 + 24) % 24;
    const auto& T = kFoF2Decile[upper ? 1 : 0][s][ri];
    return T[i0][h] + t * (T[i1][h] - T[i0][h]);
}
double geomagneticLatDeg(double lat, double lon) {
    const double g0 = 78.5 * kDeg, l0 = -68.2 * kDeg;
    return std::asin(clampd(std::sin(g0) * std::sin(lat * kDeg) + std::cos(g0) * std::cos(lat * kDeg) * std::cos(lon * kDeg - l0), -1, 1)) / kDeg;
}
double r12FromSfi(double sfi) {
    const double a = 0.00089, b = 0.728, c = 63.7 - sfi;
    const double r = (-b + std::sqrt(std::max(b * b - 4 * a * c, 0.0))) / (2 * a);
    return std::max(r, 0.0);
}
double sfiFromR12(double r) { return 63.7 + 0.728 * r + 0.00089 * r * r; }

Noise noise(double f, int env, double lat, double lon, int year, int month, int day, double utc, double foF2Rx, bool atmo) {
    // P.372-18 Table 1 (c, d) and Table 2 (decile deviations with time) for man-made noise; galactic 52 - 23 log f above foF2 (2 dB
    // deciles); atmospheric noise from the propagation module's P.372 fit (its deciles taken as 9 / 7 dB)
    static const double c[4] = {53.6, 67.2, 72.5, 76.8}, dd[4] = {28.6, 27.7, 27.7, 27.7};
    static const double du[4] = {9.2, 9.2, 10.6, 11.0}, dl[4] = {4.6, 4.6, 5.3, 6.7};
    const int e = std::clamp(env, 0, 3);
    Noise n;
    const double lf = std::log10(std::max(f, 0.01));
    n.manDb = c[e] - dd[e] * lf;
    double pm = std::pow(10.0, n.manDb / 10), pg = 0, pa = 0;
    if (f > foF2Rx) { n.galDb = 52 - 23 * lf; pg = std::pow(10.0, n.galDb / 10); }
    if (atmo) { n.atmDb = atmosphericFa(f, lat, lon, year, month, day, utc); pa = std::pow(10.0, n.atmDb / 10); }
    const double tot = pm + pg + pa;
    n.faDb = 10 * std::log10(tot);
    n.du = (pm * du[e] + pg * 2 + pa * 9) / tot;
    n.dl = (pm * dl[e] + pg * 2 + pa * 7) / tot;
    return n;
}

Prediction predict(const Path& p) {
    Prediction o;
    const GC g(p.txLat, p.txLon, p.rxLat, p.rxLon);
    o.distKm = g.D;
    if (!(g.D > 1) || !(p.fMHz > 0)) return o;
    const double r12 = std::max(p.r12, 0.0), sfi12 = p.sfi12 > 0 ? p.sfi12 : sfiFromR12(r12);
    double Es = -999, El = -999;
    if (g.D < 9000) Es = shortPathField(p, g, r12, sfi12, o);
    if (g.D > 7000) El = longPathField(p, g, r12, sfi12, o);
    double E;
    if (g.D <= 7000) { E = Es; o.method = 0; }
    else if (g.D >= 9000) { E = El; o.method = 1; }
    else {                                                                                                       // eq. (42)
        const double Xs = std::pow(10.0, 0.01 * Es), Xl = std::pow(10.0, 0.01 * El);
        E = 100 * std::log10(std::max(Xs + (g.D - 7000) / 2000 * (Xl - Xs), 1e-30));
        o.method = 2;
    }
    o.fieldDbuv = E;
    o.prDbw = E + p.rxGainDbi - 20 * std::log10(p.fMHz) - 107.2;                                                 // eq. (43)
    o.n = noise(p.fMHz, p.noiseEnv, p.rxLat, p.rxLon, p.year, p.month, p.day, p.utcHour,
                foF2(p.month, p.rxLat, p.rxLon, p.utcHour, r12), p.atmosphericNoise);
    double fa = o.n.faDb;
    if (p.extraNoiseDb > -90) fa = 10 * std::log10(std::pow(10.0, fa / 10) + std::pow(10.0, p.extraNoiseDb / 10));
    o.snrDb = o.prDbw - fa - 10 * std::log10(std::max(p.bandwidthHz, 1.0)) + 204;                               // eq. (45)
    // section 8: deciles (WARC HFBC-87 within the hour: lower 8, upper 5 dB; P.842 day to day by f / basic MUF)
    double ld, ud;
    const bool hiLat = std::fabs(geomagneticLatDeg(p.txLat, p.txLon)) >= 60 || std::fabs(geomagneticLatDeg(p.rxLat, p.rxLon)) >= 60;
    signalDeciles(p.fMHz / std::max(o.basicMufMHz, 0.1), hiLat, ld, ud);
    const double DL = std::sqrt(8.0 * 8.0 + ld * ld + o.n.du * o.n.du), DU = std::sqrt(5.0 * 5.0 + ud * ud + o.n.dl * o.n.dl);   // eq. (46)
    o.snrLowerDb = o.snrDb - DL; o.snrUpperDb = o.snrDb + DU;
    // section 10.1 / P.842 Table 1: reliability from the median and the decile deviations
    const double x = o.snrDb - p.requiredSnrDb;
    o.reliability = x >= 0 ? std::min(1.0, (130 - 80 / (1 + x / DL)) / 100) : std::max(0.0, (80 / (1 - x / DU) - 30) / 100);
    // section 3.6: probability of ionospheric support (operational MUF as MUF(50))
    const double mu = std::max(o.opMufMHz, 0.1), r = p.fMHz / mu;
    const double dlo = o.owfMHz / mu, dhi = o.hpfMHz / mu;
    if (r <= 1) o.mufProb = std::min(1.0, 1.3 - 0.8 / (1 + (1 - r) / std::max(1 - dlo, 0.01)));               // eq. (9)
    else o.mufProb = std::max(0.0, 0.8 / (1 + (r - 1) / std::max(dhi - 1, 0.01)) - 0.3);                       // eq. (10)
    o.valid = true;
    return o;
}

Chart chart(Path p, const std::vector<double>& fr) {
    Chart c;
    c.freqMHz = fr;
    const size_t nf = fr.size();
    c.rel.assign(24 * nf, 0); c.snr.assign(24 * nf, -99);
    for (int h = 0; h < 24; h++) {
        p.utcHour = h;
        double luf = 0;
        for (size_t i = 0; i < nf; i++) {
            p.fMHz = fr[i];
            const Prediction q = predict(p);
            c.rel[h * nf + i] = (float)q.reliability;
            c.snr[h * nf + i] = (float)q.snrDb;
            if (i == 0) { c.muf[h] = q.opMufMHz; c.owf[h] = q.owfMHz; }
            if (luf == 0 && q.snrDb >= p.requiredSnrDb && fr[i] <= q.opMufMHz) luf = fr[i];
        }
        c.luf[h] = luf;
    }
    return c;
}

std::vector<double> hamBandsMHz() { return {1.84, 3.6, 5.35, 7.1, 10.12, 14.1, 18.1, 21.1, 24.9, 28.2}; }

} // namespace dect2::prop::hf
