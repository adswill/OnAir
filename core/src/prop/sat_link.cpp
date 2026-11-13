// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Earth-space link geometry and budget (see sat_link.h).
#include "dect2/prop/sat_link.h"
#include "dect2/prop/itu_atmos.h"
#include <algorithm>
#include <cmath>

namespace dect2::prop::sat {
namespace {
constexpr double kPi = 3.14159265358979323846, kDeg = kPi / 180.0;
constexpr double kRe = 6371.0, kA = 6378.137, kE2 = 0.00669437999014, kMu = 398600.4418;
}

Look look(double la, double lo, double hk, double sla, double slo, double salt) {
    const double p = la * kDeg, l = lo * kDeg;
    const double N = kA / std::sqrt(1 - kE2 * std::sin(p) * std::sin(p));
    const double x = (N + hk) * std::cos(p) * std::cos(l), y = (N + hk) * std::cos(p) * std::sin(l), z = (N * (1 - kE2) + hk) * std::sin(p);
    const double r = kA + salt, sp = sla * kDeg, sl = slo * kDeg;
    const double dx = r * std::cos(sp) * std::cos(sl) - x, dy = r * std::cos(sp) * std::sin(sl) - y, dz = r * std::sin(sp) - z;
    const double e = -std::sin(l) * dx + std::cos(l) * dy;
    const double n = -std::sin(p) * std::cos(l) * dx - std::sin(p) * std::sin(l) * dy + std::cos(p) * dz;
    const double u = std::cos(p) * std::cos(l) * dx + std::cos(p) * std::sin(l) * dy + std::sin(p) * dz;
    Look o;
    o.rangeKm = std::sqrt(dx * dx + dy * dy + dz * dz);
    o.elevDeg = std::atan2(u, std::hypot(e, n)) / kDeg;
    o.azDeg = std::fmod(std::atan2(e, n) / kDeg + 360.0, 360.0);
    return o;
}

double orbitalSpeedKmS(double alt) { return std::sqrt(kMu / (kRe + std::max(alt, 0.0))); }

double slantRangeKm(double alt, double el) {
    const double R = kRe, s = std::sin(el * kDeg), c = std::cos(el * kDeg);
    return std::sqrt((R + alt) * (R + alt) - R * R * c * c) - R * s;
}

double rangeRateKmS(double alt, double el) {
    const double R = kRe;
    const double gamma = kPi / 2 - el * kDeg - std::asin(std::clamp(R * std::cos(el * kDeg) / (R + alt), -1.0, 1.0));
    return orbitalSpeedKmS(alt) * R * std::sin(gamma) / std::max(slantRangeKm(alt, el), 1e-3);
}

double dopplerHz(double fMHz, double rr) { return fMHz * 1e6 * rr / 299792.458; }

Budget budget(const Link& l) {
    Budget b;
    const double f = std::max(l.fGHz, 0.01), el = std::clamp(l.elevDeg, 0.1, 90.0);
    b.fsplDb = 92.45 + 20 * std::log10(f) + 20 * std::log10(std::max(l.rangeKm, 1.0));
    b.gasDb = f >= 1 ? itu::slantGasDb(f, l.stHkm, el, l.rho0) : 0;
    const double p = std::clamp(100 - l.availabilityPct, 0.001, 50.0);
    b.rainDb = f >= 1 ? itu::rainSlantDb(f, el, l.stLat, l.stHkm, l.rainR001, std::min(p, 5.0), l.tauDeg, l.rainHeightKm) : 0;
    b.scintDb = f >= 4 && el >= 5 ? itu::scintillationDb(f, el, l.antennaM, p, l.nWet, l.efficiency) : 0;
    b.atmDb = itu::totalEarthSpaceDb(b.gasDb, b.rainDb, 0, b.scintDb);
    b.totalLossDb = b.fsplDb + b.atmDb;
    auto noiseK = [&](double absorbDb) {
        const double t = std::pow(10.0, -absorbDb / 10);
        const double sky = 275.0 * (1 - t) + 2.7 * t;                    // P.618-14 section 3
        const double Lf = std::pow(10.0, std::max(l.rxFeederDb, 0.0) / 10);
        const double Trx = 290.0 * (std::pow(10.0, l.rxNoiseFigureDb / 10) - 1);
        return std::make_pair(sky, (sky + l.groundPickupK) / Lf + 290.0 * (1 - 1 / Lf) + Trx);
    };
    const auto nk = noiseK(b.gasDb + b.rainDb);
    b.skyK = nk.first; b.tsysK = nk.second;
    const double g = l.rxGainDbi - l.rxFeederDb;
    b.gtDbK = g - 10 * std::log10(b.tsysK);
    b.rxDbw = l.eirpDbw - b.totalLossDb + g;
    b.cn0DbHz = l.eirpDbw - b.totalLossDb + b.gtDbK + 228.6;
    b.cnDb = b.cn0DbHz - 10 * std::log10(std::max(l.bandwidthHz, 1.0));
    b.marginDb = b.cnDb - l.requiredSnrDb;
    const auto nc = noiseK(b.gasDb);
    b.clearSkyCnDb = l.eirpDbw - b.fsplDb - b.gasDb + g - 10 * std::log10(nc.second) + 228.6 - 10 * std::log10(std::max(l.bandwidthHz, 1.0));
    return b;
}

} // namespace dect2::prop::sat
