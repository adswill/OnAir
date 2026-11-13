// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Propagation prediction, see core/include/dect2/prop/prop.h and docs/propagation.md.
#include "dect2/prop/prop.h"
#include "dect2/prop/prop_terrain.h"
#include "dect2/prop/hf_p533.h"
#include "dect2/prop/itu_atmos.h"
#include "dect2/prop/p1812.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace dect2::prop {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRe = 6371.0;           // km
constexpr double kAe = 8495.0;           // km, 4/3 earth radius
constexpr double kDeg = kPi / 180.0;
constexpr double kHfMax = 30.0;          // MHz: below this the HF model, from this up the VHF/UHF model
constexpr double kF2HeightKm = 300.0, kDHeightKm = 100.0;
constexpr double kMinElevDeg = 3.0;      // lowest useful skywave take-off angle
constexpr double kGyroMHz = 1.2;         // gyrofrequency term of the absorption formula (mid latitudes)
constexpr double kExcessSkyDb = 8.7;     // P.533 "excess system loss" (polarisation coupling, focusing, what the simple model leaves out)
constexpr double kNone = 1e9;

double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
double groundSigma(Ground g) {   // S/m
    switch (g) { case Ground::Sea: return 5.0; case Ground::Wet: return 0.01; case Ground::Dry: return 0.001; default: return 0.005; }
}
double groundEps(Ground g) {     // relative permittivity
    switch (g) { case Ground::Sea: return 80.0; case Ground::Wet: return 30.0; case Ground::Dry: return 4.0; default: return 15.0; }
}
bool isLinear(Pol p) { return p == Pol::Vertical || p == Pol::Horizontal; }
double hopSlantKm(double hopKm, double hKm) {   // path length of one hop up to a layer and down
    const double th = hopKm / (2 * kRe);
    return 2 * std::sqrt(kRe * kRe + (kRe + hKm) * (kRe + hKm) - 2 * kRe * (kRe + hKm) * std::cos(th));
}
double powerSumLossDb(double a, double b) {   // the loss of two paths whose powers add
    return -10 * std::log10(std::pow(10.0, -a / 10) + std::pow(10.0, -b / 10));
}
}

// ---------------------------------------------------------------- antenna pattern hook
double AntennaPattern::gainDbi(double azOffsetDeg, double elDeg) const {
    if (!valid()) return 0;
    double a = std::fmod(azOffsetDeg, 360.0);
    if (a < 0) a += 360.0;
    const int i0 = (int)a % 360, i1 = (i0 + 1) % 360;
    const double t = a - std::floor(a);
    double g = azDb[i0] + (azDb[i1] - azDb[i0]) * t;
    if (elDb.size() == 181) {
        const double e = clampd(elDeg, -90, 90) + 90.0;
        const int j0 = std::min(179, (int)e);
        const double u = e - j0;
        g += elDb[j0] + (elDb[j0 + 1] - elDb[j0]) * u;
    }
    return g;
}

bool parseAntennaPattern(const std::string& text, AntennaPattern& out, std::string& err) {
    std::vector<std::pair<double, double>> az, el;
    std::istringstream in(text);
    std::string line;
    int ln = 0;
    bool firstContent = true;   // a header line ("azimuth,gain") may come first
    while (std::getline(in, line)) {
        ln++;
        const size_t h = line.find('#');
        if (h != std::string::npos) line.resize(h);
        for (char& c : line) if (c == ';' || c == '\t') c = ',';
        size_t s = line.find_first_not_of(" \r");
        if (s == std::string::npos) continue;
        line = line.substr(s);
        bool isEl = false;
        if (line.rfind("el,", 0) == 0) { isEl = true; line = line.substr(3); }
        char* e1 = nullptr;
        const double a = std::strtod(line.c_str(), &e1);
        const bool wasFirst = firstContent;
        firstContent = false;
        if (e1 == line.c_str()) { if (wasFirst && !isEl) continue; err = "line " + std::to_string(ln) + ": not a number"; return false; }
        while (*e1 == ' ' || *e1 == ',') e1++;
        char* e2 = nullptr;
        const double g = std::strtod(e1, &e2);
        if (e2 == e1) { err = "line " + std::to_string(ln) + ": two numbers wanted (angle, gain)"; return false; }
        if (!std::isfinite(a) || !std::isfinite(g) || g < -100 || g > 60) { err = "line " + std::to_string(ln) + ": gain out of range"; return false; }
        (isEl ? el : az).push_back({a, g});
    }
    if (az.size() < 2) { err = "fewer than two azimuth lines (\"azimuth_deg,gain_dbi\")"; return false; }
    auto table = [](std::vector<std::pair<double, double>> v, double lo, double span, int n, bool wrap) {
        for (auto& x : v) if (wrap) { x.first = std::fmod(x.first, 360.0); if (x.first < 0) x.first += 360.0; }
        std::sort(v.begin(), v.end());
        std::vector<double> t(n);
        for (int i = 0; i < n; i++) {
            const double ang = lo + span * i / (wrap ? n : n - 1);
            size_t k = 0;
            while (k < v.size() && v[k].first < ang) k++;
            std::pair<double, double> a, b;
            if (k == 0) { a = wrap ? std::make_pair(v.back().first - 360.0, v.back().second) : v.front(); b = v.front(); }
            else if (k == v.size()) { a = v.back(); b = wrap ? std::make_pair(v.front().first + 360.0, v.front().second) : v.back(); }
            else { a = v[k - 1]; b = v[k]; }
            t[i] = b.first == a.first ? a.second : a.second + (b.second - a.second) * (ang - a.first) / (b.first - a.first);
        }
        return t;
    };
    out.azDb = table(az, 0, 360, 360, true);
    out.elDb.clear();
    if (!el.empty()) out.elDb = table(el, -90, 180, 181, false);
    return true;
}

double eirpDbw(const Params& p) { return p.txPowerDbw + p.txGainDbi - p.txFeederDb; }

// ---------------------------------------------------------------- basics
double fsplDb(double fMHz, double dKm) { return 32.45 + 20 * std::log10(std::max(fMHz, 1e-6)) + 20 * std::log10(std::max(dKm, 1e-6)); }
double ktbDbm(double bandwidthHz) { return -174.0 + 10 * std::log10(std::max(bandwidthHz, 1.0)); }

double manMadeFa(double fMHz, Env env) {
    double c = 72.5, d = 27.7;
    switch (env) {
        case Env::City: c = 76.8; d = 27.7; break;
        case Env::Residential: c = 72.5; d = 27.7; break;
        case Env::Rural: c = 67.2; d = 27.7; break;
        case Env::Quiet: c = 53.6; d = 28.6; break;
    }
    return c - d * std::log10(std::max(fMHz, 0.01));
}

double galacticFa(double fMHz) {
    const double g = 52.0 - 23.0 * std::log10(std::max(fMHz, 0.01));
    // the ionosphere keeps cosmic noise out below its critical frequency: fade it in between 5 and 12 MHz
    const double w = clampd((fMHz - 5.0) / 7.0, 0.0, 1.0);
    return w <= 0 ? -100.0 : g + 10 * std::log10(w);
}

double atmosphericFa(double fMHz, double lat, double lon, int year, int month, int day, double utcHour) {
    if (fMHz >= 30) return -100.0;
    const double f = std::max(fMHz, 0.1), lf = std::log10(f);
    // night and day medians for summer at about 40 degrees, fitted to the shape of the P.372 maps
    const double night = f <= 10 ? 90.0 - 38.0 * lf : 52.0 - 60.0 * std::log10(f / 10.0);
    const double dayv = f <= 10 ? 45.0 - 10.0 * lf : 35.0 - 50.0 * std::log10(f / 10.0);
    const double c = solarCosZenith(lat, lon, year, month, day, utcHour);
    const double t = clampd((c + 0.1) / 0.2, 0.0, 1.0);   // 0 night .. 1 day, across sunrise / sunset
    double fa = night + (dayv - night) * t;
    fa += 10.0 - 20.0 * clampd((std::fabs(lat) - 20.0) / 40.0, 0.0, 1.0);   // tropics +10, 40 degrees 0, polar -10
    const double s = std::cos(2 * kPi * (month - 7) / 12.0) * (lat >= 0 ? 1.0 : -1.0);   // +1 local summer, -1 local winter
    fa += 5.0 * s - 5.0;                                                                 // summer 0, winter -10
    return fa;
}

double externalNoiseFa(double fMHz, Env env) {
    const double s = std::pow(10.0, manMadeFa(fMHz, env) / 10.0) + std::pow(10.0, galacticFa(fMHz) / 10.0);
    return std::max(0.0, 10 * std::log10(s));
}

double cascadeNfDb(double feederDb, double radioNfDb, bool preamp, double preampGainDb, double preampNfDb) {
    const double L = std::pow(10.0, std::max(0.0, feederDb) / 10.0), Fr = std::pow(10.0, std::max(0.0, radioNfDb) / 10.0);
    const double chain = L * Fr;   // feeder (F = L) then the radio
    if (!preamp) return 10 * std::log10(chain);
    const double Fp = std::pow(10.0, std::max(0.0, preampNfDb) / 10.0), Gp = std::pow(10.0, preampGainDb / 10.0);
    return 10 * std::log10(Fp + (chain - 1.0) / std::max(Gp, 1e-3));
}

double systemNoiseDbm(double bandwidthHz, double faDb, double sysNfDb) {
    const double fa = std::pow(10.0, faDb / 10.0), f = std::pow(10.0, std::max(0.0, sysNfDb) / 10.0);
    return ktbDbm(bandwidthHz) + 10 * std::log10(fa + f - 1.0);
}

double systemNoiseDbm(double fMHz, double bandwidthHz, double nfDb, Env env) {
    return systemNoiseDbm(bandwidthHz, externalNoiseFa(fMHz, env), nfDb);
}

double fieldDbuvm(double rxDbm, double fMHz, double rxGainDbi) { return rxDbm - rxGainDbi + 20 * std::log10(fMHz) + 77.2; }
double dbmToUv50(double dbm) { return std::sqrt(std::pow(10.0, dbm / 10.0) * 1e-3 * 50.0) * 1e6; }

double radioHorizonKm(double h1M, double h2M) { return 4.12 * (std::sqrt(std::max(h1M, 0.0)) + std::sqrt(std::max(h2M, 0.0))); }

double planeEarthLossDb(double dKm, double h1M, double h2M) {
    return 40 * std::log10(std::max(dKm, 1e-6) * 1000.0) - 20 * std::log10(std::max(h1M, 0.5) * std::max(h2M, 0.5));
}

double twoRayLossDb(double fMHz, double dKm, double h1M, double h2M) {
    const double d = std::max(dKm, 1e-6) * 1000.0, h1 = std::max(h1M, 0.0), h2 = std::max(h2M, 0.0);
    const double delta = std::sqrt(d * d + (h1 + h2) * (h1 + h2)) - std::sqrt(d * d + (h1 - h2) * (h1 - h2));
    const double k = 2 * kPi * fMHz / 299.792458;
    const double a = std::max(std::fabs(2 * std::sin(k * delta / 2)), 1e-3);   // nulls floored at -60 dB
    return fsplDb(fMHz, dKm) - 20 * std::log10(a);
}

double knifeEdgeJ(double v) {
    if (v <= -0.78) return 0;
    return 6.9 + 20 * std::log10(std::sqrt((v - 0.1) * (v - 0.1) + 1) + v - 0.1);
}

double fresnelV(double hM, double d1Km, double d2Km, double fMHz) {
    const double lambda = 299.792458 / fMHz;
    const double d1 = std::max(d1Km, 1e-6) * 1000.0, d2 = std::max(d2Km, 1e-6) * 1000.0;
    return hM * std::sqrt(2.0 / lambda * (1.0 / d1 + 1.0 / d2));
}

double smoothEarthResidueDb(double fMHz, double dKm, double h1M, double h2M, double aeKm, Ground g, Pol pol) {
    const double eps = groundEps(g), sig = groundSigma(g), x = 18000.0 * sig / fMHz;
    const double Kh = 0.36 * std::pow(aeKm * fMHz, -1.0 / 3.0) * std::pow((eps - 1) * (eps - 1) + x * x, -0.25);
    const double K = pol == Pol::Horizontal ? Kh : Kh * std::sqrt(eps * eps + x * x);   // circular: the vertical part decides
    const double K2 = K * K, K4 = K2 * K2;
    const double beta = (1 + 1.6 * K2 + 0.67 * K4) / (1 + 4.5 * K2 + 1.53 * K4);
    const double X = std::max(2.2 * beta * std::cbrt(fMHz) * std::pow(aeKm, -2.0 / 3.0) * dKm, 1e-6);
    const double F = X >= 1.6 ? 11 + 10 * std::log10(X) - 17.6 * X : -20 * std::log10(X) - 5.6488 * std::pow(X, 1.425);
    auto G = [&](double hM) {
        const double B = beta * 9.6e-3 * std::pow(fMHz, 2.0 / 3.0) * std::pow(aeKm, -1.0 / 3.0) * std::max(hM, 0.0);
        const double v = B > 2 ? 17.6 * std::sqrt(B - 1.1) - 5 * std::log10(B - 1.1) - 8 : 20 * std::log10(std::max(B + 0.1 * B * B * B, 1e-9));
        return std::max(v, 2 + 20 * std::log10(K));
    };
    return -(F + G(h1M) + G(h2M));
}

double diffractionLossDb(double fMHz, double dKm, double h1M, double h2M, Ground g, Pol pol) {
    const double h1 = std::max(h1M, 0.5) / 1000.0, h2 = std::max(h2M, 0.5) / 1000.0, d = std::max(dKm, 1e-3);   // km
    const double dlos = std::sqrt(2 * kAe) * (std::sqrt(h1) + std::sqrt(h2));
    if (d >= dlos) return std::max(0.0, smoothEarthResidueDb(fMHz, d, h1M, h2M, kAe, g, pol));
    // inside the horizon: the smallest clearance of the ray over the curved earth against 0.552 of the first Fresnel zone (P.526 3.2)
    const double c = (h1 - h2) / (h1 + h2), m = d * d / (4 * kAe * (h1 + h2));
    const double arg = clampd(1.5 * c * std::sqrt(3 * m / std::pow(m + 1, 3)), -1.0, 1.0);
    const double b = 2 * std::sqrt((m + 1) / (3 * m)) * std::cos(kPi / 3 + std::acos(arg) / 3);
    const double d1 = d / 2 * (1 + b), d2 = d - d1;
    const double hClear = ((h1 - d1 * d1 / (2 * kAe)) * d2 + (h2 - d2 * d2 / (2 * kAe)) * d1) / d * 1000.0;   // m
    const double lambda = 299.792458 / fMHz;
    const double hreq = 0.552 * std::sqrt(d1 * 1000.0 * d2 * 1000.0 * lambda / (d * 1000.0));
    if (hClear > hreq) return 0;
    const double aem = 0.5 * std::pow(d / (std::sqrt(h1) + std::sqrt(h2)), 2);
    const double Ah = smoothEarthResidueDb(fMHz, d, h1M, h2M, aem, g, pol);
    if (Ah < 0) return 0;
    return (1 - hClear / hreq) * Ah;
}

double troposcatterLossDb(double fMHz, double dKm, double h1M, double h2M, double gainsDbi, double thetaMrad) {
    const double fG = std::max(fMHz, 30.0) / 1000.0, d = std::max(dKm, 1.0);
    const double Lf = 25 * std::log10(fG) - 2.5 * std::pow(std::log10(fG / 2), 2);
    // angular distance (mrad) over a smooth 4/3 earth: the path angle less the two horizon depressions
    const double theta = thetaMrad >= 0 ? thetaMrad
        : std::max(0.0, 1000.0 * d / kAe - 1000.0 * (std::sqrt(2 * std::max(h1M, 0.5) / 1000.0 / kAe) + std::sqrt(2 * std::max(h2M, 0.5) / 1000.0 / kAe)));
    const double Lc = 0.051 * std::exp(0.055 * std::max(gainsDbi, 0.0));   // aperture-to-medium coupling
    const double N0 = 325.0;                                                // sea-level refractivity, temperate
    return 190.1 + Lf + 20 * std::log10(d) + 0.573 * theta - 0.15 * N0 + Lc;
}

double clutterLossDb(double fMHz, double hM, Clutter c) {
    if (c == Clutter::Open || fMHz < 30) return 0;
    const double R = c == Clutter::Suburban ? 10.0 : c == Clutter::DenseUrban ? 20.0 : 15.0;   // representative clutter height, m
    if (hM >= R) return 0;
    const double fG = clampd(fMHz, 30, 3000) / 1000.0;
    const double hdif = R - std::max(hM, 0.0), theta = std::atan(hdif / 27.0) / kDeg;   // ws = 27 m street width
    const double v = 0.342 * std::sqrt(fG) * std::sqrt(hdif * theta);
    return std::max(0.0, knifeEdgeJ(v) - 6.03);
}

double rainSpecificDbKm(double fMHz, double rainMmH) {
    // 4.0: the full ITU-R P.838-3 regression (horizontal polarisation, terrestrial path); itu::rainGammaDbKm takes any tilt
    if (rainMmH <= 0 || fMHz < 1000) return 0;
    return itu::rainGammaDbKm(fMHz / 1000.0, rainMmH, 0, 0);
}

double rainLossDb(double fMHz, double dKm, double rainMmH) {
    const double g = rainSpecificDbKm(fMHz, rainMmH);
    if (g <= 0) return 0;
    const double d0 = 35.0 * std::exp(-0.015 * std::min(rainMmH, 100.0));   // rain cells are a few km wide
    return g * dKm / (1 + dKm / d0);
}

double gasSpecificDbKm(double fMHz) {
    // 4.0: ITU-R P.676-13 Annex 1 line by line, sea level, 15 C, 7.5 g/m3 (was a hand-drawn curve)
    if (fMHz < 1000) return 0;   // below 1 GHz it is under 0.01 dB/km, and P.1812 (VHF/UHF) leaves it out
    return itu::gammaGasDbKm(fMHz / 1000.0);
}

double polLossDb(Pol tx, Pol rx) {
    if (tx == rx) return 0;
    if (isLinear(tx) && isLinear(rx)) return 20;   // crossed linear: the practical cross-polar isolation of simple antennas
    if (isLinear(tx) != isLinear(rx)) return 3;    // linear to circular
    return 20;                                     // opposite circular senses
}

double groundWaveFactor(double fMHz, double dKm, Ground g, Pol pol) {
    const double lambda = 299.792458 / fMHz, x = 18000.0 * groundSigma(g) / fMHz, eps = groundEps(g);
    double p = kPi * dKm * 1000.0 / (lambda * x);
    if (pol == Pol::Horizontal) p /= std::cos(std::atan((eps - 1) / x));
    else p *= std::cos(std::atan((eps + 1) / x));
    return (2 + 0.3 * p) / (2 + p + 0.6 * p * p);
}

double groundWaveLossDb(double fMHz, double dKm, Ground g, Pol pol, double h1M, double h2M) {
    const double d = std::max(dKm, 0.01);
    const double flat = fsplDb(fMHz, d) - 20 * std::log10(std::max(groundWaveFactor(fMHz, d, g, pol), 1e-12));
    const double curved = fsplDb(fMHz, d) + smoothEarthResidueDb(fMHz, d, h1M, h2M, kAe, g, pol);
    return std::max(flat, curved);   // flat-earth Norton close in, the earth's curvature (P.526) beyond
}

// ---------------------------------------------------------------- geodesy
double greatCircleKm(double lat1, double lon1, double lat2, double lon2) {
    const double a1 = lat1 * kDeg, a2 = lat2 * kDeg, dl = (lon2 - lon1) * kDeg, dp = a2 - a1;
    const double h = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(a1) * std::cos(a2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * kRe * std::asin(std::min(1.0, std::sqrt(h)));
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * kDeg, p2 = lat2 * kDeg, dl = (lon2 - lon1) * kDeg;
    const double b = std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl)) / kDeg;
    return std::fmod(b + 360.0, 360.0);
}

void destination(double lat, double lon, double bearingDeg, double distKm, double& lat2, double& lon2) {
    const double d = distKm / kRe, b = bearingDeg * kDeg, p1 = lat * kDeg, l1 = lon * kDeg;
    const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b));
    const double l2 = l1 + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2));
    lat2 = p2 / kDeg;
    lon2 = std::fmod(l2 / kDeg + 540.0, 360.0) - 180.0;
}

void midpoint(double lat1, double lon1, double lat2, double lon2, double& lat, double& lon) {
    const double p1 = lat1 * kDeg, p2 = lat2 * kDeg, l1 = lon1 * kDeg, dl = (lon2 - lon1) * kDeg;
    const double bx = std::cos(p2) * std::cos(dl), by = std::cos(p2) * std::sin(dl);
    lat = std::atan2(std::sin(p1) + std::sin(p2), std::sqrt((std::cos(p1) + bx) * (std::cos(p1) + bx) + by * by)) / kDeg;
    lon = std::fmod((l1 + std::atan2(by, std::cos(p1) + bx)) / kDeg + 540.0, 360.0) - 180.0;
}

// ---------------------------------------------------------------- ionosphere
double sfiToSunspot(double sfi) {   // inverts SFI = 63.7 + 0.728 R + 0.00089 R^2
    const double s = std::max(sfi, 63.7);
    return (-0.728 + std::sqrt(0.728 * 0.728 + 4 * 0.00089 * (s - 63.7))) / (2 * 0.00089);
}

double solarCosZenith(double lat, double lon, int y, int m, int d, double utcHour) {
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    const int mm = (int)clampd(m, 1, 12);
    const int doy = cum[mm - 1] + (int)clampd(d, 1, 31) + (leap && mm > 2 ? 1 : 0);
    const double decl = 23.44 * kDeg * std::sin(2 * kPi * (doy - 81) / 365.0);
    const double H = (15.0 * (utcHour - 12.0) + lon) * kDeg;
    return std::sin(lat * kDeg) * std::sin(decl) + std::cos(lat * kDeg) * std::cos(decl) * std::cos(H);
}

double foF2(double lat, double lon, const Params& p) {
    const double R = std::min(sfiToSunspot(p.sfi), 250.0);
    const double latF = 1.0 - 0.35 * std::pow(std::sin(lat * kDeg), 2);
    const double day = (5.5 + 0.045 * R) * latF, night = 0.45 * day;
    const double c = clampd((solarCosZenith(lat, lon, p.year, p.month, p.day, p.utcHour) + 0.1) / 1.1, 0.0, 1.0);
    const double storm = 1.0 - 0.04 * clampd(p.kIndex - 3.0, 0.0, 6.0);   // a geomagnetic storm depresses the F2 layer
    return (night + (day - night) * std::sqrt(c)) * storm;
}

double hopElevationDeg(double hopKm, double layerKm) {
    const double th = hopKm / (2 * kRe);
    return std::atan2(std::cos(th) - kRe / (kRe + layerKm), std::sin(th)) / kDeg;
}

double hopMaxKm(double layerKm, double minElevDeg) {
    const double e = minElevDeg * kDeg;
    const double half = kPi / 2 - e - std::asin(kRe * std::cos(e) / (kRe + layerKm));
    return 2 * kRe * half;
}

double hopIncidenceSec(double hopKm, double layerKm) {
    const double th = hopKm / (2 * kRe);
    const double t = kRe * std::sin(th) / ((kRe + layerKm) - kRe * std::cos(th));   // tan(incidence angle at the layer)
    return std::sqrt(1 + t * t);
}

double dLayerSec(double hopKm, double reflectKm, double dKm) {
    const double e = hopElevationDeg(hopKm, reflectKm) * kDeg;
    const double s = kRe * std::cos(e) / (kRe + dKm);   // sin of the angle from the vertical where the ray crosses the D layer
    return 1.0 / std::sqrt(std::max(1 - s * s, 1e-6));
}

double dLayerAbsorptionDb(double fMHz, double hopKm, double cosChi, double sunspot) {
    const double chi = std::acos(clampd(cosChi, -1, 1));
    const double I = chi >= kPi / 2 ? 0.0 : std::pow(std::cos(0.881 * chi), 1.3);
    const double Ir = (1 + 0.0037 * sunspot) * std::max(I, 0.1);   // a night floor
    return 677.2 * Ir * dLayerSec(hopKm, kF2HeightKm, kDHeightKm) / (std::pow(fMHz + kGyroMHz, 1.98) + 10.2);
}

double aboveMufLossDb(double fMHz, double muf) {
    if (muf <= 0) return kNone;
    if (fMHz <= muf) return 0;
    const double r = fMHz / muf - 1;
    return 130.0 * r * r;
}

double mufMHz(double lat, double lon, double distKm, const Params& p) {
    const double hm = hopMaxKm(kF2HeightKm, kMinElevDeg);
    const double hop = distKm / std::max(1.0, std::ceil(distKm / hm));
    return foF2(lat, lon, p) * hopIncidenceSec(hop, kF2HeightKm);
}

// ---------------------------------------------------------------- one path
namespace {
double patternGain(const Params& p, double brg, double el) {
    if (p.txPattern && p.txPattern->valid()) return p.txPattern->gainDbi(brg - p.txAzDeg, el);
    return p.txGainDbi;
}

Point evalPath(const Params& p, double distKm, double brg, double rxLat, double rxLon, double midLat, double midLon) {
    Point o;
    o.distKm = distKm;
    o.bearingDeg = brg;
    const double d = std::max(distKm, 0.01);
    const double f = std::max(p.fMHz, 0.01);
    o.fsDb = fsplDb(f, d);
    o.horizonKm = radioHorizonKm(p.txHeightM, p.rxHeightM);
    double rxRel = 0;   // received power relative to the transmitter power into the feeder, dB (gains and losses)
    if (f >= kHfMax) {
        double L = o.fsDb;
        Model m = Model::LineOfSight;
        if (p.pathModel == PathModel::Auto) {
            // 4.0: ITU-R P.1812-8 over the terrain profile, or over a flat sea-level profile when no terrain covers the path
            p1812::Input in;
            bool terr = false;
            if (p.terrain && !p.terrain->empty()) {
                const int ns = (int)clampd(d / 0.1, 16, 200);
                const Profile pr = pathProfile(*p.terrain, p.txLat, p.txLon, rxLat, rxLon, ns);
                if (pr.complete && pr.dKm.size() >= 3) { in.dKm = pr.dKm; in.hM = pr.hM; terr = true; }
            }
            if (!terr) {
                const int ns = (int)clampd(d / 0.5, 16, 100);
                for (int i = 0; i <= ns; i++) { in.dKm.push_back(d * i / ns); in.hM.push_back(0.0); }
            }
            in.fGHz = f / 1000.0; in.timePct = p.timePct; in.locPct = p.locPct;
            in.txHeightM = std::max(p.txHeightM, 0.5); in.rxHeightM = std::max(p.rxHeightM, 0.5);
            in.vertical = p.txPol != Pol::Horizontal; in.latDeg = midLat; in.deltaN = p.deltaN; in.n0 = p.n0;
            in.seaFraction = p.seaFraction >= 0 ? p.seaFraction : (p.ground == Ground::Sea ? 1.0 : 0.0);
            in.sigmaLocDb = p.sigmaLocDb;
            const p1812::Result r = p1812::compute(in);
            if (r.valid) {
                L = r.lb;
                o.kFactor = r.ae / kRe; o.diffDb = r.ldp; o.tropoDb = r.lbs; o.ductDb = r.lba; o.beta0 = r.beta0;
                m = r.mechanism == 2 ? Model::Troposcatter : r.mechanism == 3 ? Model::Ducting
                  : r.mechanism == 1 ? (terr ? Model::Terrain : Model::Diffraction) : Model::LineOfSight;
            }
            o.terrainUsed = terr;
            o.flatEarth = !terr;
        } else {
        double diff = 0, plane = 0, theta = -1;
        bool terr = false;
        if (p.terrain && !p.terrain->empty()) {
            const int ns = (int)clampd(d / 0.1, 16, 200);
            const Profile pr = pathProfile(*p.terrain, p.txLat, p.txLon, rxLat, rxLon, ns);
            if (pr.complete) {
                const TerrainLoss tl = terrainLoss(pr, p.txHeightM, p.rxHeightM, f);
                diff = tl.diffractionDb;
                plane = planeEarthLossDb(d, tl.effTxM, tl.effRxM);
                theta = tl.thetaMrad;
                terr = true;
            }
        }
        if (!terr) {
            diff = diffractionLossDb(f, d, p.txHeightM, p.rxHeightM, p.ground, p.txPol);
            plane = planeEarthLossDb(d, p.txHeightM, p.rxHeightM);
        }
        o.terrainUsed = terr;
        const Model dm = terr ? Model::Terrain : Model::Diffraction;
        switch (p.pathModel) {
            case PathModel::FreeSpace: m = Model::FreeSpace; break;
            case PathModel::SmoothEarth: L = o.fsDb + diff; m = diff > 0.5 ? dm : Model::LineOfSight; break;
            case PathModel::TwoRayExact: {
                const double tr = d < o.horizonKm && !terr ? twoRayLossDb(f, d, p.txHeightM, p.rxHeightM) : 0;
                L = std::max(o.fsDb + diff, tr);
                m = o.fsDb + diff >= tr ? (diff > 0.5 ? dm : Model::LineOfSight) : Model::TwoRay;
                break;
            }
            case PathModel::Legacy39: default: {
                L = std::max(o.fsDb + diff, plane);
                m = o.fsDb + diff >= plane ? (diff > 0.5 ? dm : Model::LineOfSight) : Model::TwoRay;
                // troposcatter adds its power to the diffracted field; it carries far beyond the horizon
                const double ts = troposcatterLossDb(f, d, p.txHeightM, p.rxHeightM, p.txGainDbi + p.rxGainDbi, theta);
                if (ts < L + 1) m = ts < L ? Model::Troposcatter : m;
                L = powerSumLossDb(L, ts);
                break;
            }
        }
        }
        o.model = m;
        o.excessDb = L - o.fsDb;
        o.clutterDb = clutterLossDb(f, p.rxHeightM, p.rxClutter);
        o.rainDb = rainLossDb(f, d, p.rainMmH);
        o.gasDb = p.gas ? gasSpecificDbKm(f) * d : 0;
        o.polDb = polLossDb(p.txPol, p.rxPol);
        o.txGainDbi = patternGain(p, brg, 0);
        o.pathLossDb = L + o.clutterDb + o.rainDb + o.gasDb + o.polDb;
        rxRel = o.txGainDbi - p.txFeederDb - o.pathLossDb;
    } else {
        // ground wave
        double gw = kNone;
        if (p.hfModel != HfModel::SkyOnly) gw = groundWaveLossDb(f, d, p.ground, p.txPol, std::max(p.txHeightM, 0.0), std::max(p.rxHeightM, 0.0));
        // skywave: the fewest F2 hops with a take-off angle of at least 3 degrees
        double sw = kNone;
        double skyGainOverride = -999;   // the gain used by the P.533 prediction (pattern at the mode's take-off angle)
        const double hm = hopMaxKm(kF2HeightKm, kMinElevDeg);
        const int n = (int)std::max(1.0, std::ceil(d / hm));
        const double hop = d / n;
        o.muf = mufMHz(midLat, midLon, d, p);
        o.elevDeg = hopElevationDeg(hop, kF2HeightKm);
        if (p.hfModel != HfModel::GroundOnly && !p.hfLegacy) {
            // 4.0: ITU-R P.533-14 with the P.1239-4 maps (hf_p533.h)
            hf::Path hp;
            hp.txLat = p.txLat; hp.txLon = p.txLon; hp.rxLat = rxLat; hp.rxLon = rxLon;
            hp.year = p.year; hp.month = p.month; hp.day = p.day; hp.utcHour = p.utcHour;
            hp.r12 = p.r12 >= 0 ? p.r12 : hf::r12FromSfi(p.sfi);
            hp.sfi12 = p.sfi;
            hp.fMHz = f;
            hp.txPowerDbw = p.txPowerDbw - p.txFeederDb;
            hp.txGainDbi = patternGain(p, brg, 10);   // a typical take-off angle; refined below with the strongest mode's
            hp.rxGainDbi = p.rxGainDbi;
            hp.bandwidthHz = p.bandwidthHz; hp.requiredSnrDb = p.requiredSnrDb;
            hp.noiseEnv = (int)p.env; hp.atmosphericNoise = p.atmosphericNoise;
            const double nf = cascadeNfDb(p.rxFeederDb, p.rxNfDb, p.preamp, p.preampGainDb, p.preampNfDb);
            hp.extraNoiseDb = 10 * std::log10(std::max(std::pow(10.0, nf / 10) - 1, 1e-9));   // the receive chain, as noise above kTB
            hf::Prediction q = hf::predict(hp);
            if (q.valid && q.bestMode >= 0 && p.txPattern && p.txPattern->valid()) {
                hp.txGainDbi = patternGain(p, brg, q.modes[q.bestMode].elevDeg);
                q = hf::predict(hp);
            }
            if (q.valid && q.prDbw > -900) {
                const double eirp = hp.txPowerDbw + hp.txGainDbi;
                sw = eirp - (q.prDbw - p.rxGainDbi);   // the isotropic basic loss equivalent to the P.533 available power
                o.muf = q.opMufMHz; o.hfOwf = q.owfMHz; o.hfHpf = q.hpfMHz; o.hfMufProb = q.mufProb;
                o.hfReliability = q.reliability; o.hfMethod = q.method;
                if (q.bestMode >= 0) {
                    const hf::Mode& bm = q.modes[q.bestMode];
                    o.elevDeg = bm.elevDeg; o.hops = bm.hops; o.absorptionDb = bm.absorptionDb; o.aboveMufDb = bm.aboveMufDb; o.hfLayer = bm.layer;
                } else { o.hops = std::max(1, (int)std::ceil(d / 4000.0)); o.elevDeg = 3; }
                skyGainOverride = hp.txGainDbi;
            }
        } else if (p.hfModel != HfModel::GroundOnly) {
            o.aboveMufDb = aboveMufLossDb(f, o.muf);
            if (o.aboveMufDb < 40) {
                const double R = sfiToSunspot(p.sfi);
                const double cz = solarCosZenith(midLat, midLon, p.year, p.month, p.day, p.utcHour);
                double La = dLayerAbsorptionDb(f, hop, cz, R);
                if (std::fabs(midLat) > 55 && p.kIndex >= 5) La += 3.0 * (p.kIndex - 4);   // auroral absorption in a storm (rough)
                o.hops = n;
                o.absorptionDb = n * La;
                sw = fsplDb(f, n * hopSlantKm(hop, kF2HeightKm)) + o.absorptionDb + o.aboveMufDb + 2.0 * (n - 1) + kExcessSkyDb;
            }
        }
        o.groundLossDb = gw;
        o.skyLossDb = sw;
        const double gG = patternGain(p, brg, 0), gS = skyGainOverride > -900 ? skyGainOverride : patternGain(p, brg, o.elevDeg);
        const double tG = gG - gw, tS = gS - sw;   // relative received power of each mode
        const double sum = 10 * std::log10(std::pow(10.0, tG / 10) + std::pow(10.0, tS / 10));
        const bool sky = tS > tG;
        o.model = sky ? Model::HfSky : Model::HfGround;
        o.txGainDbi = sky ? gS : gG;
        o.pathLossDb = std::min(o.txGainDbi - sum, kNone);
        o.excessDb = o.pathLossDb - o.fsDb;
        rxRel = o.txGainDbi - p.txFeederDb - o.pathLossDb;
    }
    o.eirpDbw = p.txPowerDbw + o.txGainDbi - p.txFeederDb;
    o.rxDbm = p.txPowerDbw + 30.0 + rxRel + p.rxGainDbi;
    o.fieldDbuvm = fieldDbuvm(o.rxDbm, f, p.rxGainDbi);
    double fa = std::pow(10.0, manMadeFa(f, p.env) / 10) + std::pow(10.0, galacticFa(f) / 10);
    if (p.atmosphericNoise) fa += std::pow(10.0, atmosphericFa(f, rxLat, rxLon, p.year, p.month, p.day, p.utcHour) / 10);
    o.faDb = std::max(0.0, 10 * std::log10(fa));   // never below 290 K: the antenna also sees the warm ground
    o.sysNfDb = cascadeNfDb(p.rxFeederDb, p.rxNfDb, p.preamp, p.preampGainDb, p.preampNfDb);
    o.noiseDbm = systemNoiseDbm(p.bandwidthHz, o.faDb, o.sysNfDb);
    o.snrDb = o.rxDbm - o.noiseDbm;
    o.marginDb = o.snrDb - p.requiredSnrDb;
    return o;
}
}

Point evaluate(const Params& p, double distKm, double brg) {
    double la, lo, ml, mo;
    destination(p.txLat, p.txLon, brg, distKm, la, lo);
    midpoint(p.txLat, p.txLon, la, lo, ml, mo);
    return evalPath(p, distKm, brg, la, lo, ml, mo);
}

Point evaluateAt(const Params& p, double lat, double lon) {
    double ml, mo;
    midpoint(p.txLat, p.txLon, lat, lon, ml, mo);
    return evalPath(p, greatCircleKm(p.txLat, p.txLon, lat, lon), bearingDeg(p.txLat, p.txLon, lat, lon), lat, lon, ml, mo);
}

Range findRange(const Params& p, double brg, double maxKm) {
    Range r;
    const int steps = 600;
    const double d0 = 0.05, d1 = std::max(maxKm, 1.0);
    bool in = false;
    for (int i = 0; i <= steps; i++) {
        const double d = d0 * std::pow(d1 / d0, (double)i / steps);
        const bool ok = evaluate(p, d, brg).marginDb >= 0;
        if (ok) {
            if (!r.any) { r.any = true; r.minKm = i == 0 ? 0 : d; }
            if (!in) r.spans.push_back({i == 0 ? 0.0 : d, d});
            r.spans.back().second = d;
            r.maxKm = d;
        }
        in = ok;
    }
    return r;
}

Grid computeGrid(const Params& p, double radiusKm, int n, const std::atomic<bool>* cancel) {
    Grid g;
    n = std::max(2, n);
    g.n = n;
    g.rangeKm = radiusKm;
    const double dLat = std::min(radiusKm / 111.19, 80.0);
    const double dLon = std::min(dLat / std::max(0.05, std::cos(clampd(p.txLat, -85, 85) * kDeg)), 180.0);
    g.lat0 = clampd(p.txLat - dLat, -85, 85); g.lat1 = clampd(p.txLat + dLat, -85, 85);
    g.lon0 = p.txLon - dLon; g.lon1 = p.txLon + dLon;
    g.snr.resize((size_t)n * n);
    g.model.resize((size_t)n * n);
    for (int y = 0; y < n; y++) {
        if (cancel && cancel->load(std::memory_order_relaxed)) { g = Grid(); return g; }
        for (int x = 0; x < n; x++) {
            const double la = g.lat0 + (g.lat1 - g.lat0) * (y + 0.5) / n;
            double lo = g.lon0 + (g.lon1 - g.lon0) * (x + 0.5) / n;
            lo = std::fmod(lo + 540.0, 360.0) - 180.0;
            const Point q = evaluateAt(p, la, lo);
            g.snr[(size_t)y * n + x] = (float)std::max(q.snrDb, -300.0);
            g.model[(size_t)y * n + x] = (unsigned char)q.model;
        }
    }
    return g;
}

const char* modelName(const Params& p) {
    if (p.fMHz < kHfMax) return p.hfLegacy ? "HF: ground wave + one-layer skywave estimate (3.9)" : "HF: ground wave + ITU-R P.533-14 skywave (P.1239-4 maps)";
    switch (p.pathModel) {
        case PathModel::Auto: return "ITU-R P.1812-8 (time and location statistics) + P.676 gases + P.838 rain";
        case PathModel::FreeSpace: return "free space (ITU-R P.525)";
        case PathModel::SmoothEarth: return "smooth-earth diffraction (ITU-R P.526)";
        case PathModel::TwoRayExact: return "exact two-ray ground reflection";
        case PathModel::Legacy39: break;
    }
    return "VHF/UHF: smooth earth (P.526) + ground reflection (3.9)";
}

std::string modelLimits(const Params& p) {
    if (p.fMHz < kHfMax && !p.hfLegacy)
        return "HF below 30 MHz: Norton ground wave plus the ITU-R P.533-14 sky-wave method: monthly median foF2 and M(3000)F2 from the CCIR "
               "numerical maps of P.1239-4 for the month, hour and sunspot number, foE, E and F2 modes (up to 3 + 6), E-layer screening, "
               "D-layer absorption at every penetration point, auroral loss, the composite method beyond 7000 km, P.372 noise and the P.842 "
               "reliability from the day-to-day spread of signal and noise. Monthly medians: a given day can differ by several MHz and 10 dB; "
               "no sporadic E, no storm-time ionosphere (the K index is not used), the antenna gain is one value for all take-off angles "
               "unless a pattern file is loaded.";
    if (p.fMHz < kHfMax)
        return "HF below 30 MHz (3.9 model): Norton ground wave (with the earth's curvature) plus a one-layer F2 skywave (foF2 from time of day, season, "
               "latitude, the SFI and K index you enter; D-layer absorption). An estimate within a few MHz and 10 dB or more. No E layer, sporadic E, "
               "tilts, auroral or equatorial effects beyond a rough storm term, no land/sea mixing on the ground wave, no terrain.";
    if (p.pathModel == PathModel::Auto) {
        std::string s = "30 MHz and up: ITU-R P.1812-8 (validated against the ITU's own test profiles): line of sight with multipath / focusing, "
                        "delta-Bullington diffraction over the profile for the median and the super-refractive earth radius, troposcatter, "
                        "ducting and layer reflection, for the time and location percentages you set; P.676-13 gases, P.838-3 rain. ";
        s += p.terrain && !p.terrain->empty() ? "Terrain: your elevation tiles (paths outside them use a flat sea-level profile). "
                                              : "NO TERRAIN LOADED: every path is a flat sea-level profile, so hills are ignored and real coverage "
                                                "is usually shorter (download tiles in Advanced > Terrain). ";
        s += "Receiver clutter from the P.2108 class; no building penetration, no sporadic E; valid 30 MHz..6 GHz (used above with P.452's "
             "mechanisms), 0.25..3000 km.";
        return s;
    }
    std::string s = "30 MHz and up: free space, the ground reflection (plane earth) and P.526 smooth-earth diffraction, which blends in before "
                    "the radio horizon and grows beyond it. ";
    s += p.terrain && !p.terrain->empty() ? "Terrain: from your elevation file (delta-Bullington). " : "No terrain (load an elevation file in Advanced). ";
    s += "No buildings beyond the receiver clutter class, no ducting or sporadic E.";
    return s;
}

const char* pointModelName(Model m) {
    switch (m) {
        case Model::FreeSpace: return "free space";
        case Model::LineOfSight: return "line of sight (free space)";
        case Model::Diffraction: return "smooth-earth diffraction";
        case Model::HfGround: return "HF ground wave";
        case Model::HfSky: return "HF skywave";
        case Model::TwoRay: return "line of sight with ground reflection";
        case Model::Terrain: return "terrain diffraction";
        case Model::Troposcatter: return "troposcatter";
        case Model::Ducting: return "ducting / layer reflection";
    }
    return "";
}

} // namespace dect2::prop
