// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radiosonde ascent sounding and the landing prediction before burst (3.9, docs/SMART5-avia-3.9.md). Pure functions of the track.
//
// Sounding: the ascent part of the track (up to the highest point) is cut into layers (500 m); each layer's wind is the least squares
// slope of position against time inside it (the balloon drifts with the air), its temperature the mean of the frames in it. From the
// temperatures: the lapse rate per layer, the freezing level (lowest 0 C crossing) and the tropopause by the WMO rule (lowest level
// above 5 km where the lapse rate falls to 2 K/km or less and stays at or below 2 K/km on average over the next 2 km). The burst is the
// highest point once the sonde has fallen 300 m below it.
//
// Landing before burst: the rest of the climb to an assumed burst height at the current ascent rate, then the fall under the parachute,
// whose speed grows with height as the air thins (v = v0 * sqrt(rho0 / rho), scale height 7 km). Each layer drifts with the wind measured
// in it on the way up; above the highest layer measured so far the wind of the top 2 km is used (a guess, and the prediction says so).
// The burst height is the one thing that is not measured: 30 km by default (a 600-800 g balloon), the user can set it.
#pragma once
#include "sonde_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct SondeLayer {
    double baseM = 0;            // the layer is baseM .. baseM + layerM
    bool hasWind = false; double windFromDeg = 0, windMs = 0, ve = 0, vn = 0;   // ve, vn: the air's motion, m/s east / north
    bool hasTemp = false; double tempC = 0;
    double lapseKkm = 0; bool hasLapse = false;   // cooling with height, K/km (to the next layer with a temperature)
    int points = 0;
};

struct SondeSounding {
    double layerM = 500;
    std::vector<SondeLayer> layers;   // ascent layers from the lowest point heard, lowest first
    double topM = 0;             // highest point so far
    double ascentMs = 0;         // mean rate of climb over the last 5 minutes of the ascent
    bool burst = false; double burstM = 0;
    bool hasFreezing = false; double freezingM = 0;
    bool hasTropo = false; double tropoM = 0, tropoTempC = 0;
};

namespace sondeprof_detail {
inline double eastM(double lon, double lon0, double lat0) { return (lon - lon0) * M_PI / 180 * 6371000.0 * std::cos(lat0 * M_PI / 180); }
inline double northM(double lat, double lat0) { return (lat - lat0) * M_PI / 180 * 6371000.0; }
inline double tOf(const std::vector<SondeTrackPoint>& tr, size_t i, double dtGuess) { return tr[i].unixT ? (double)tr[i].unixT : i * dtGuess; }
}

inline SondeSounding sondeSounding(const std::vector<SondeTrackPoint>& tr, double layerM = 500) {
    using namespace sondeprof_detail;
    SondeSounding s; s.layerM = layerM;
    if (tr.size() < 4 || layerM <= 0) return s;
    size_t top = 0; for (size_t i = 0; i < tr.size(); i++) if (tr[i].altM > tr[top].altM) top = i;
    s.topM = tr[top].altM;
    s.burst = top + 1 < tr.size() && tr.back().altM < s.topM - 300;
    if (s.burst) s.burstM = s.topM;
    double lowest = tr[0].altM; for (size_t i = 0; i <= top; i++) lowest = std::min(lowest, (double)tr[i].altM);
    const double base0 = std::floor(lowest / layerM) * layerM;
    const int nL = (int)std::floor((s.topM - base0) / layerM) + 1;
    const double lat0 = tr[0].lat, lon0 = tr[0].lon;
    const bool timed = tr[top].unixT != 0 && tr[0].unixT != 0;   // the winds and the climb need the sonde's time
    const double tRef = tOf(tr, 0, 1.0);   // (relative times: squares of Unix seconds lose the slope in double rounding)
    for (int L = 0; L < nL; L++) {
        SondeLayer ly; ly.baseM = base0 + L * layerM;
        double st = 0, se = 0, sn = 0, stt = 0, ste = 0, stn = 0, tsum = 0; int n = 0, nt = 0; double t0 = 0, t1 = 0;
        for (size_t i = 0; i <= top; i++) {
            if (tr[i].altM < ly.baseM || tr[i].altM >= ly.baseM + layerM) continue;
            const double t = tOf(tr, i, 1.0) - tRef, e = eastM(tr[i].lon, lon0, lat0), nn = northM(tr[i].lat, lat0);
            if (!n) t0 = t;
            t1 = t; n++; st += t; se += e; sn += nn; stt += t * t; ste += t * e; stn += t * nn;
            if (tr[i].tempC > -900) { tsum += tr[i].tempC; nt++; }
        }
        ly.points = n;
        if (timed && n >= 3 && t1 - t0 >= 20) {
            const double den = n * stt - st * st;
            if (den > 0) {
                ly.ve = (n * ste - st * se) / den; ly.vn = (n * stn - st * sn) / den;
                ly.windMs = std::hypot(ly.ve, ly.vn);
                double d = std::atan2(-ly.ve, -ly.vn) * 180 / M_PI; if (d < 0) d += 360; ly.windFromDeg = d;
                ly.hasWind = true;
            }
        }
        if (nt) { ly.hasTemp = true; ly.tempC = tsum / nt; }
        s.layers.push_back(ly);
    }
    // lapse rate, freezing level, tropopause
    std::vector<size_t> ti; for (size_t i = 0; i < s.layers.size(); i++) if (s.layers[i].hasTemp) ti.push_back(i);
    auto mid = [&](size_t i) { return s.layers[i].baseM + layerM / 2; };
    for (size_t k = 0; k + 1 < ti.size(); k++) {
        SondeLayer& a = s.layers[ti[k]]; const SondeLayer& b = s.layers[ti[k + 1]];
        a.lapseKkm = -(b.tempC - a.tempC) / ((mid(ti[k + 1]) - mid(ti[k])) / 1000); a.hasLapse = true;
        if (!s.hasFreezing && a.tempC >= 0 && b.tempC < 0) { s.hasFreezing = true; s.freezingM = mid(ti[k]) + a.tempC / (a.tempC - b.tempC) * (mid(ti[k + 1]) - mid(ti[k])); }
    }
    for (size_t k = 0; k + 1 < ti.size() && !s.hasTropo; k++) {
        const double z = mid(ti[k]); if (z < 5000) continue;
        if (!s.layers[ti[k]].hasLapse || s.layers[ti[k]].lapseKkm > 2) continue;
        bool ok = true; int above = 0;
        for (size_t j = k + 1; j < ti.size() && mid(ti[j]) - z <= 2000 + 1e-6; j++) {
            if (-(s.layers[ti[j]].tempC - s.layers[ti[k]].tempC) / ((mid(ti[j]) - z) / 1000) > 2) { ok = false; break; }
            above++;
        }
        if (ok && above >= 2) { s.hasTropo = true; s.tropoM = z; s.tropoTempC = s.layers[ti[k]].tempC; }
    }
    // ascent rate: over the last 300 s of the climb
    if (timed) {   size_t a = top; const double tt = tOf(tr, top, 1.0);
        while (a > 0 && tt - tOf(tr, a - 1, 1.0) <= 300) a--;
        const double dt = tt - tOf(tr, a, 1.0);
        if (dt > 30) s.ascentMs = (tr[top].altM - tr[a].altM) / dt; }
    return s;
}

struct SondeEarlyLanding {
    bool ok = false;
    double lat = 0, lon = 0;
    double secondsToLanding = 0;
    double burstM = 0;           // the burst height used
    double guessedFromM = 0;     // above this height the wind is a guess (the top of what was measured)
    std::string text;
};

// lat, lon, altM: the sonde now. burstM: the assumed burst height (> altM). v0: descent speed at sea level (m/s, typically 5 - 6).
inline SondeEarlyLanding predictSondeLandingAscent(const std::vector<SondeTrackPoint>& tr, double lat, double lon, double altM, double burstM = 30000,
                                                   double v0 = 5.5, double groundM = 0) {
    SondeEarlyLanding r;
    const SondeSounding s = sondeSounding(tr);
    if (s.burst || s.ascentMs < 1.0 || burstM <= altM || s.layers.empty()) return r;
    const double L = s.layerM;
    // the wind of the top 2 km measured, for everything above
    double gE = 0, gN = 0; int gn = 0; double measuredTop = 0;
    for (const auto& ly : s.layers) if (ly.hasWind) measuredTop = std::max(measuredTop, ly.baseM + L);
    for (const auto& ly : s.layers) if (ly.hasWind && ly.baseM + L > measuredTop - 2000) { gE += ly.ve; gN += ly.vn; gn++; }
    if (!gn) return r;
    gE /= gn; gN /= gn;
    auto wind = [&](double z, double& e, double& n) {
        if (z >= measuredTop) { e = gE; n = gN; return; }
        const int i = (int)std::floor((z - s.layers[0].baseM) / L);
        if (i >= 0 && i < (int)s.layers.size() && s.layers[i].hasWind) { e = s.layers[i].ve; n = s.layers[i].vn; return; }
        // a layer with no fit: the nearest one that has one
        for (int d = 1; d < (int)s.layers.size(); d++)
            for (int j : {i - d, i + d}) if (j >= 0 && j < (int)s.layers.size() && s.layers[j].hasWind) { e = s.layers[j].ve; n = s.layers[j].vn; return; }
        e = gE; n = gN;
    };
    double E = 0, N = 0, T = 0;
    const double step = 50;   // metres of height per step
    for (double z = altM; z < burstM; z += step) {   // the rest of the climb
        const double dz = std::min(step, burstM - z), dt = dz / s.ascentMs; double e, n; wind(z + dz / 2, e, n);
        E += e * dt; N += n * dt; T += dt;
    }
    for (double z = burstM; z > groundM; z -= step) {   // the fall
        const double dz = std::min(step, z - groundM), zm = z - dz / 2;
        const double v = v0 * std::exp(zm / 7000.0 / 2);   // sqrt(rho0 / rho) with rho = rho0 exp(-z / 7000)
        const double dt = dz / v; double e, n; wind(zm, e, n);
        E += e * dt; N += n * dt; T += dt;
    }
    r.ok = true;
    r.lat = lat + N / 6371000.0 * 180 / M_PI;
    r.lon = lon + E / (6371000.0 * std::cos(lat * M_PI / 180)) * 180 / M_PI;
    r.secondsToLanding = T; r.burstM = burstM; r.guessedFromM = measuredTop;
    char b[200];
    snprintf(b, sizeof b, "landing in about %.0f min if it bursts at %.0f km; wind above %.1f km is extrapolated", T / 60, burstM / 1000, measuredTop / 1000);
    r.text = b;
    return r;
}

} // namespace dect2
