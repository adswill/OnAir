// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Antenna designer: calculators (wavelength, wire lengths, cable loss, SWR / return loss, matching networks, line transforms, EIRP,
// antenna factor) and the band presets. Public formulas; docs/ANTENNA.md.
#include "dect2/antd/antd_tools.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2::antd {

double wavelength(double fHz, double vf) { return kC0 / std::max(fHz, 1.0) * vf; }

WireLengths wireLengths(double fHz) {
    const double fm = std::max(fHz, 1.0) / 1e6;
    WireLengths w;
    w.halfDipole = 142.65 / fm;
    w.quarterVertical = 71.3 / fm;
    w.fullLoop = 306.3 / fm;
    w.efhw = 0.49 * 299.792458 / fm;
    w.fiveEighths = 0.625 * 0.95 * 299.792458 / fm;
    w.inverted_v = 146.0 / fm;
    return w;
}

const std::vector<Cable>& cables() {
    // matched loss k1 sqrt(f) + k2 f, dB per 100 m with f in MHz: fitted to typical datasheet values at 10 / 100 / 1000 MHz (approximate)
    static const std::vector<Cable> c = {
        {"RG-174", 50, 0.66, 2.60, 0.035, "thin, short jumpers only"},
        {"RG-58", 50, 0.66, 1.39, 0.021, ""},
        {"RG-8X (mini-8)", 50, 0.78, 1.00, 0.015, ""},
        {"H155", 50, 0.79, 0.83, 0.0038, ""},
        {"RG-213", 50, 0.66, 0.60, 0.009, ""},
        {"LMR-240", 50, 0.84, 0.85, 0.0, ""},
        {"LMR-400", 50, 0.85, 0.39, 0.0012, ""},
        {"Belden 9913", 50, 0.84, 0.41, 0.0014, ""},
        {"Aircell 7", 50, 0.83, 0.62, 0.0065, ""},
        {"Ecoflex 10", 50, 0.86, 0.38, 0.002, ""},
        {"Ecoflex 15", 50, 0.86, 0.27, 0.0015, ""},
        {"LMR-600", 50, 0.87, 0.25, 0.0011, ""},
        {"1/2\" Heliax (LDF4-50A)", 50, 0.88, 0.19, 0.0009, ""},
        {"7/8\" Heliax (LDF5-50A)", 50, 0.89, 0.11, 0.0005, ""},
        {"RG-6 (75 ohm TV)", 75, 0.85, 0.60, 0.0013, ""},
        {"300-ohm twin lead", 300, 0.82, 0.25, 0.003, "dry; much worse when wet"},
        {"450-ohm window line", 450, 0.91, 0.10, 0.002, "dry; much worse when wet"},
        {"600-ohm open wire", 600, 0.97, 0.05, 0.001, ""},
    };
    return c;
}

double cableLossDb(const Cable& c, double fHz, double lengthM) {
    const double fm = std::max(fHz, 0.0) / 1e6;
    return (c.k1 * std::sqrt(fm) + c.k2 * fm) * lengthM / 100.0;
}

LineLoss mismatchedLineLoss(double matchedDb, double swrLoad) {
    LineLoss r;
    r.matchedDb = std::max(0.0, matchedDb);
    r.swrLoad = std::max(1.0, swrLoad);
    const double a = std::pow(10.0, r.matchedDb / 10), g = (r.swrLoad - 1) / (r.swrLoad + 1);
    r.totalDb = 10 * std::log10((a * a - g * g) / (a * (1 - g * g)));
    if (!std::isfinite(r.totalDb)) r.totalDb = 99;
    const double gi = g / a;
    r.swrIn = gi >= 0.999999 ? 1e6 : (1 + gi) / (1 - gi);
    return r;
}

namespace {
Mismatch fromGamma(double g) {
    Mismatch m;
    m.gamma = std::min(std::max(g, 0.0), 1.0);
    m.swr = m.gamma >= 0.999999 ? 1e6 : (1 + m.gamma) / (1 - m.gamma);
    m.returnLossDb = m.gamma > 0 ? -20 * std::log10(m.gamma) : 99;
    m.mismatchLossDb = m.gamma < 1 ? -10 * std::log10(1 - m.gamma * m.gamma) : 99;
    m.reflectedPct = 100 * m.gamma * m.gamma;
    return m;
}
} // namespace

Mismatch fromSwr(double swr) { swr = std::max(1.0, swr); return fromGamma((swr - 1) / (swr + 1)); }
Mismatch fromZ(cd z, double z0) { return fromGamma(std::abs((z - z0) / (z + z0))); }
Mismatch fromReturnLoss(double rl) { return fromGamma(std::pow(10.0, -std::max(0.0, rl) / 20)); }

std::string reactancePart(double x, double fHz) {
    const double w = 2 * kPi * std::max(fHz, 1.0);
    char b[64];
    if (std::fabs(x) < 1e-9) return "none";
    if (x > 0) {
        const double l = x / w;
        if (l >= 1e-3) std::snprintf(b, sizeof b, "%.3g mH", l * 1e3);
        else if (l >= 1e-6) std::snprintf(b, sizeof b, "%.3g uH", l * 1e6);
        else std::snprintf(b, sizeof b, "%.3g nH", l * 1e9);
    } else {
        const double c = -1 / (w * x);
        if (c >= 1e-6) std::snprintf(b, sizeof b, "%.3g uF", c * 1e6);
        else if (c >= 1e-9) std::snprintf(b, sizeof b, "%.3g nF", c * 1e9);
        else std::snprintf(b, sizeof b, "%.3g pF", c * 1e12);
    }
    return b;
}

std::vector<LMatch> lNetwork(cd zl, double z0, double fHz) {
    std::vector<LMatch> out;
    const double R = zl.real(), X = zl.imag();
    if (R <= 0 || z0 <= 0) return out;
    if (R > z0) {   // shunt across the load, then series (Pozar 5.1)
        const double q = std::sqrt(R / z0) * std::sqrt(std::max(0.0, R * R + X * X - z0 * R));
        for (int s : {1, -1}) {
            const double B = (X + s * q) / (R * R + X * X);
            if (std::fabs(B) < 1e-15) continue;
            LMatch m; m.ok = true; m.shuntAtLoad = true;
            m.xShunt = -1 / B;
            m.xSeries = 1 / B + X * z0 / R - z0 / (B * R);
            out.push_back(m);
        }
    } else {        // series with the load, then shunt at the source side
        const double q = std::sqrt(std::max(0.0, R * (z0 - R)));
        for (int s : {1, -1}) {
            LMatch m; m.ok = true; m.shuntAtLoad = false;
            m.xSeries = s * q - X;
            const double B = s * std::sqrt((z0 - R) / R) / z0;
            m.xShunt = std::fabs(B) > 1e-15 ? -1 / B : 1e12;
            out.push_back(m);
        }
    }
    for (auto& m : out) { m.series = reactancePart(m.xSeries, fHz); m.shunt = std::fabs(m.xShunt) >= 1e11 ? "none" : reactancePart(m.xShunt, fHz); }
    return out;
}

std::vector<StubMatch> stubMatch(cd zl, double z0, double fHz, double vf, bool open) {
    std::vector<StubMatch> out;
    const double R = zl.real(), X = zl.imag();
    if (R <= 0 || z0 <= 0) return out;
    std::vector<double> ts;
    if (std::fabs(R - z0) < 1e-9) ts.push_back(-X / (2 * z0));
    else {
        const double q = std::sqrt(R * ((z0 - R) * (z0 - R) + X * X) / z0);
        ts.push_back((X + q) / (R - z0)); ts.push_back((X - q) / (R - z0));
    }
    const double lam = wavelength(fHz, vf);
    for (double t : ts) {
        StubMatch s; s.open = open;
        s.dLambda = t >= 0 ? std::atan(t) / (2 * kPi) : (kPi + std::atan(t)) / (2 * kPi);
        const double B = (R * R * t - (z0 - X * t) * (X + z0 * t)) / (z0 * (R * R + (X + z0 * t) * (X + z0 * t)));
        double l = open ? -std::atan(B * z0) / (2 * kPi) : (std::fabs(B) > 1e-15 ? std::atan(1 / (B * z0)) / (2 * kPi) : 0.25);
        if (l < 0) l += 0.5;
        s.lLambda = l;
        s.dM = s.dLambda * lam; s.lM = s.lLambda * lam;
        out.push_back(s);
    }
    return out;
}

QuarterWave quarterWave(double rl, double z0, double fHz, double vf) {
    QuarterWave q;
    q.z = std::sqrt(std::max(0.0, rl * z0));
    q.lengthM = wavelength(fHz, vf) / 4;
    return q;
}

Gamma gammaMatch(double fHz, double d) {
    Gamma g;
    const double lam = wavelength(fHz);
    g.rodLengthM = 0.045 * lam;
    g.rodSpacingM = std::max(0.007 * lam, 2 * d);
    g.rodDiameterRatio = 1.0 / 3;
    g.capPf = 7.0 * lam;   // about 7 pF per metre of wavelength
    return g;
}

Choke balunChoke(double fHz, double zHigh, double zLow, double chokeOhms) {
    Choke c;
    c.turnsRatio = zLow > 0 ? std::sqrt(std::max(zHigh, 0.0) / zLow) : 0;
    c.reactance = chokeOhms;
    c.inductanceH = chokeOhms / (2 * kPi * std::max(fHz, 1.0));
    return c;
}

cd lineTransform(cd zl, double z0, double len, double fHz, double vf, double lossDbPerM) {
    const double beta = 2 * kPi * fHz / (kC0 * std::max(vf, 0.01));
    const cd gl = cd(std::max(lossDbPerM, 0.0) / 8.685889638, beta) * len;
    const cd t = std::tanh(gl);
    return z0 * (zl + z0 * t) / (z0 + zl * t);
}

void seriesToParallel(cd zs, double& rp, double& xp) {
    const double r = zs.real(), x = zs.imag(), m2 = r * r + x * x;
    rp = r != 0 ? m2 / r : 1e12;
    xp = x != 0 ? m2 / x : 1e12;
}

cd parallelToSeries(double rp, double xp) {
    cd y = 0;
    if (rp != 0 && std::isfinite(rp)) y += 1.0 / rp;
    if (xp != 0 && std::isfinite(xp)) y += cd(0, -1.0 / xp);
    return std::abs(y) > 0 ? 1.0 / y : cd(1e12, 0);
}

Eirp eirp(double txW, double lossDb, double gainDbi) {
    Eirp e;
    const double dbm = 10 * std::log10(std::max(txW, 1e-15) * 1000) - lossDb + gainDbi;
    e.eirpDbm = dbm; e.eirpW = std::pow(10.0, dbm / 10) / 1000;
    e.erpDbm = dbm - 2.15; e.erpW = std::pow(10.0, e.erpDbm / 10) / 1000;
    return e;
}

double antennaFactorDb(double fHz, double gainDbi, double z0) {
    const double lam = wavelength(fHz), g = std::pow(10.0, gainDbi / 10);
    return 20 * std::log10(std::sqrt(4 * kPi * kEta0 / (z0 * g)) / lam);
}

double gainFromAperture(double a, double fHz) {
    const double lam = wavelength(fHz);
    return 10 * std::log10(std::max(4 * kPi * a / (lam * lam), 1e-30));
}

const std::vector<BandPreset>& bandPresets() {
    static const std::vector<BandPreset> b = {
        {"160 m", 1.810e6, 2.000e6, "IARU region 1"}, {"80 m", 3.500e6, 3.800e6, "IARU region 1"}, {"60 m", 5.3515e6, 5.3665e6, "WRC-15"},
        {"40 m", 7.000e6, 7.200e6, "IARU region 1"}, {"30 m", 10.100e6, 10.150e6, ""}, {"20 m", 14.000e6, 14.350e6, ""},
        {"17 m", 18.068e6, 18.168e6, ""}, {"15 m", 21.000e6, 21.450e6, ""}, {"12 m", 24.890e6, 24.990e6, ""},
        {"CB 27 MHz", 26.965e6, 27.405e6, "40 channels"}, {"10 m", 28.000e6, 29.700e6, ""}, {"6 m", 50.000e6, 52.000e6, "IARU region 1"},
        {"4 m", 70.000e6, 70.500e6, "where allocated"}, {"FM broadcast", 87.5e6, 108.0e6, ""}, {"Airband", 118.0e6, 137.0e6, "AM voice"},
        {"Weather satellites", 137.0e6, 138.0e6, "NOAA / Meteor APT, LRPT"}, {"2 m", 144.0e6, 146.0e6, "IARU region 1"},
        {"Marine VHF", 156.0e6, 162.05e6, "AIS 161.975 / 162.025"}, {"DAB band III", 174.0e6, 240.0e6, ""},
        {"70 cm", 430.0e6, 440.0e6, "IARU region 1"}, {"ISM 433", 433.05e6, 434.79e6, ""}, {"PMR446", 446.0e6, 446.2e6, ""},
        {"DVB-T UHF", 470.0e6, 694.0e6, "channels 21-48 (Europe)"}, {"SRD / LoRa 868", 863.0e6, 870.0e6, ""},
        {"ADS-B 1090", 1085.0e6, 1095.0e6, ""}, {"23 cm", 1240.0e6, 1300.0e6, ""}, {"GPS L1", 1570.0e6, 1580.0e6, "1575.42 MHz"},
        {"L-band weather (HRPT)", 1690.0e6, 1710.0e6, ""}, {"Wi-Fi 2.4 GHz", 2400.0e6, 2483.5e6, ""},
    };
    return b;
}

} // namespace dect2::antd
