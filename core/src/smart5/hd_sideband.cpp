// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HD Radio digital sideband level (see hd_sideband.h).
#include "dect2/smart5/hd_sideband.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace smart5 {

std::string HdSidebandReport::summary() const {
    if (!valid) return "";
    char b[160];
    if (!stepDbc) snprintf(b, sizeof b, "no HD sidebands (%.1f dBc)", totalDbc);
    else snprintf(b, sizeof b, "HD sidebands %.1f dBc (step %d dBc), lower %.1f, upper %.1f dBc%s", totalDbc, stepDbc, lowerDbc, upperDbc, asymmetric ? ", asymmetric" : "");
    return b;
}

HdSidebandReport hdSidebandLevels(const std::vector<double>& p, double startHz, double binHz) {
    HdSidebandReport r;
    if (p.empty() || binHz <= 0) return r;
    auto band = [&](double lo, double hi, int* n) {
        double s = 0; int c = 0;
        for (size_t k = 0; k < p.size(); k++) {
            const double f = startHz + k * binHz;
            if (f >= lo && f < hi) { s += p[k]; c++; }
        }
        if (n) *n = c;
        return s;
    };
    const double endHz = startHz + p.size() * binHz;
    if (startHz > -198.4e3 || endHz < 198.4e3) return r;
    // noise per bin from beyond the sidebands, the quieter side (an adjacent station may sit on the other)
    double nb = 0;
    int nl = 0, nu = 0;
    const double sl = band(-240e3, -205e3, &nl), su = band(205e3, 240e3, &nu);
    if (nl && nu) nb = std::min(sl / nl, su / nu); else if (nl) nb = sl / nl; else if (nu) nb = su / nu;
    int na = 0, nlo = 0, nup = 0;
    const double an = band(-100e3, 100e3, &na) - nb * na;
    const double lo = band(-198.4e3, -129.4e3, &nlo) - nb * nlo, up = band(129.4e3, 198.4e3, &nup) - nb * nup;
    if (an <= 0) return r;
    auto dbc = [&](double x) { return x > 0 ? 10 * std::log10(x / an) : -99.0; };
    r.valid = true;
    r.lowerDbc = dbc(lo); r.upperDbc = dbc(up); r.totalDbc = dbc(std::max(0.0, lo) + std::max(0.0, up));
    r.imbalanceDb = (lo > 0 && up > 0) ? r.upperDbc - r.lowerDbc : 0;
    r.asymmetric = std::fabs(r.imbalanceDb) > 3;
    if (r.totalDbc >= -26) {
        const int steps[3] = {-20, -14, -10};
        int best = -20; double bd = 1e9;
        for (int s : steps) if (std::fabs(r.totalDbc - s) < bd) { bd = std::fabs(r.totalDbc - s); best = s; }
        r.stepDbc = best;
    }
    return r;
}

HdSidebandReport hdSidebandLevelsDb(const std::vector<float>& db, double startHz, double binHz) {
    std::vector<double> p(db.size());
    for (size_t i = 0; i < db.size(); i++) p[i] = std::pow(10.0, db[i] / 10.0);
    return hdSidebandLevels(p, startHz, binHz);
}

} // namespace smart5
} // namespace dect2
