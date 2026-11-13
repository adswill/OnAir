// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Simulated radiosonde flights for the landing-prediction tests (docs/SONDE-PREDICT-4.0.md). The truth is deliberately not the
// predictor's own model: a non-standard atmosphere, a parachute whose drag changes with height, a wind field with a boundary layer,
// shear, a jet stream and fine structure that changes in time and with place, gusts, a swing under the balloon and the parachute, GPS
// noise and glitches, dropouts, and a sonde that is lost some way above the hills it lands on.
#pragma once
#include "dect2/sonde/sonde_predict.h"
#include "dect2/sonde/sonde_tel.h"
#include <cmath>
#include <random>
#include <vector>

namespace sim {

constexpr double kPi = 3.14159265358979323846, kD = kPi / 180, kR = 6371000.0;

struct Frame { double t, lat, lon, alt, tempC, vz, hs, hd; double trueAgl; bool desc; };

struct Scenario {
    bool missAscent = false;     // first heard after the burst (high up on the way down)
    bool front = false;          // the low-level wind changes fast between ascent and descent
    bool noChute = false;
    bool hills = true;
    double lastAglMin = 150, lastAglMax = 1200;   // the last frame heard this high above the ground
};

struct Flight {
    std::vector<Frame> frames;
    double landLat = 0, landLon = 0, landT = 0, landGround = 0;
    double lat0 = 0, lon0 = 0;
    // the truth wind and ground, for the forecast and the terrain model given to the predictor
    struct Wind {
        double s0, sg, dir0, veer, jetZ, jetW, jetS, jetDir, stratS, stratDir, bl;
        double fineA[6], fineL[6], fineP[6], fineD[6];
        double chg[4], chgDir[4];   // change per hour of four smooth components (m/s / h)
        double gradE, gradN;        // change per 100 km (m/s), low levels
        double frontRate = 0;       // m/s per hour below 3 km
        double tRef = 0;
        void at(double h, double t, double eM, double nM, double& ve, double& vn, bool fine = true) const {
            const double hb = std::max(0.0, h);
            // boundary layer: power law from s0 to sg over bl metres, veering
            double sp, dir;
            if (hb < bl) { const double u = std::pow(std::max(0.01, hb / bl), 0.25); sp = s0 + (sg - s0) * u; dir = dir0 + veer * u; }
            else { sp = sg; dir = dir0 + veer; }
            double e = sp * std::sin(dir * kD), n = sp * std::cos(dir * kD);
            // jet stream (Gaussian in height), stratosphere above 20 km
            const double jet = jetS * std::exp(-0.5 * std::pow((hb - jetZ) / jetW, 2)) * std::min(1.0, hb / 3000.0);
            e += jet * std::sin(jetDir * kD); n += jet * std::cos(jetDir * kD);
            if (hb > 18000) { const double u = std::min(1.0, (hb - 18000) / 6000); e += u * stratS * std::sin(stratDir * kD); n += u * stratS * std::cos(stratDir * kD); }
            if (fine) for (int k = 0; k < 6; k++) { const double a = fineA[k] * std::sin(2 * kPi * hb / fineL[k] + fineP[k]); e += a * std::sin(fineD[k] * kD); n += a * std::cos(fineD[k] * kD); }
            // change in time: four smooth height shapes
            const double dtH = (t - tRef) / 3600.0;
            for (int k = 0; k < 4; k++) {
                const double shape = std::sin(kPi * (k + 1) * std::min(hb, 32000.0) / 32000.0 + k);
                e += dtH * chg[k] * shape * std::sin(chgDir[k] * kD); n += dtH * chg[k] * shape * std::cos(chgDir[k] * kD);
            }
            if (hb < 3000) { const double u = 1 - hb / 3000; e += dtH * frontRate * u * 0.8; n += dtH * frontRate * u * 0.6; }
            // with place (low levels)
            const double lowU = std::exp(-hb / 5000);
            e += lowU * gradE * nM / 100000.0; n += lowU * gradN * eM / 100000.0;
            ve = e; vn = n;
        }
    } wind;
    struct Ground {
        double base = 0, A[3] = {0, 0, 0}, L[3] = {1, 1, 1}, P[3] = {0, 0, 0}, D[3] = {0, 0, 0};
        double lat0 = 0, lon0 = 0;
        double at(double lat, double lon) const {
            const double e = (lon - lon0) * kD * kR * std::cos(lat0 * kD), n = (lat - lat0) * kD * kR;
            double h = base;
            for (int k = 0; k < 3; k++) { const double x = e * std::sin(D[k] * kD) + n * std::cos(D[k] * kD); h += A[k] * (1 + std::sin(2 * kPi * x / L[k] + P[k])); }
            return h;
        }
    } ground;
    // the atmosphere of the flight
    double tOff = 0, lapse = 0.0065, tropo = 11000;
    double tempK(double h) const {
        const double t0 = 288.15 + tOff;
        if (h < tropo) return t0 - lapse * h;
        const double tt = t0 - lapse * tropo;
        return h < 20000 ? tt : tt + 0.0012 * (h - 20000);
    }
    std::vector<double> rhoTab;   // every 10 m from -500 m
    void buildRho() {
        rhoTab.clear();
        double p = 101325 * std::exp(9.80665 * 500 / (287.053 * tempK(-500)));
        for (int k = 0; k <= 5200; k++) {
            const double h = -500 + 10.0 * k, T = tempK(h);
            if (k) p *= std::exp(-9.80665 * 10 / (287.053 * 0.5 * (T + tempK(h - 10))));
            rhoTab.push_back(p / (287.053 * T));
        }
    }
    double rho(double h) const {
        const double x = std::max(0.0, std::min(5199.999, (h + 500) / 10.0));
        const int i = (int)x; return rhoTab[i] + (x - i) * (rhoTab[i + 1] - rhoTab[i]);
    }
    // the forecast a model would give: no fine structure, plus an error correlated over 2 km
    dect2::sondepred::ForecastWinds forecast(double sigma, unsigned seed) const {
        std::mt19937 g(seed); std::normal_distribution<double> nd(0, 1);
        static const double zs[] = {100, 400, 750, 1000, 1500, 2000, 3000, 4200, 5500, 7200, 9200, 10400, 11800, 13600, 16200, 18500, 20600, 22000, 24000, 26500, 31000};
        dect2::sondepred::ForecastWinds F; F.source = "sim";
        const double rh = std::exp(-1.0);
        for (int c = 0; c < 3; c++) {
            dect2::sondepred::ForecastWinds::Column col;
            const double fe = c == 0 ? 0 : (c == 1 ? 25000 : -25000), fn = c == 2 ? 25000 : 0;
            col.lat = lat0 + fn / kR / kD; col.lon = lon0 + fe / (kR * std::cos(lat0 * kD)) / kD;
            double xe = nd(g), xn = nd(g);
            std::vector<double> ee, nn;
            for (double z : zs) { (void)z; ee.push_back(xe); nn.push_back(xn); xe = rh * xe + std::sqrt(1 - rh * rh) * nd(g); xn = rh * xn + std::sqrt(1 - rh * rh) * nd(g); }
            for (int k = -2; k <= 8; k++) {
                const double t = wind.tRef + 3600.0 * k;
                col.times.push_back(t);
                std::vector<dect2::sondepred::ForecastWinds::Level> L;
                for (size_t i = 0; i < sizeof zs / sizeof zs[0]; i++) {
                    double ve, vn; wind.at(zs[i], t, fe, fn, ve, vn, false);
                    L.push_back({zs[i], ve + sigma * ee[i], vn + sigma * nn[i]});
                }
                col.levels.push_back(L);
            }
            F.cols.push_back(col);
        }
        return F;
    }
};

// One flight. The truth integrates at 0.2 s; the sonde sends one frame a second.
inline Flight makeFlight(unsigned seed, const Scenario& sc) {
    std::mt19937 g(seed * 7919u + 13u);
    std::uniform_real_distribution<double> U(0, 1);
    std::normal_distribution<double> N(0, 1);
    auto u = [&](double a, double b) { return a + (b - a) * U(g); };
    Flight F;
    F.lat0 = u(30, 60); F.lon0 = u(-10, 40);
    auto& W = F.wind;
    W.s0 = u(0.5, 6); W.sg = u(4, 16); W.dir0 = u(0, 360); W.veer = u(10, 45); W.bl = u(600, 1500);
    W.jetZ = u(8500, 13000); W.jetW = u(1500, 3500); W.jetS = u(10, 65); W.jetDir = W.dir0 + u(-60, 60);
    W.stratS = u(0, 20); W.stratDir = u(0, 360);
    for (int k = 0; k < 6; k++) { W.fineA[k] = u(0.3, 1.5); W.fineL[k] = u(400, 3000); W.fineP[k] = u(0, 6.3); W.fineD[k] = u(0, 360); }
    for (int k = 0; k < 4; k++) { W.chg[k] = u(0.2, 1.2); W.chgDir[k] = u(0, 360); }
    W.gradE = u(-2, 2); W.gradN = u(-2, 2);
    W.frontRate = sc.front ? u(2.5, 5) : 0;
    W.tRef = 1.79e9 + seed * 86400.0;
    F.tOff = u(-15, 15); F.lapse = u(0.0055, 0.0075); F.tropo = u(9500, 16000);
    F.buildRho();
    auto& G = F.ground;
    G.lat0 = F.lat0; G.lon0 = F.lon0;
    if (sc.hills) { G.base = u(0, 600); for (int k = 0; k < 3; k++) { G.A[k] = u(0, 120); G.L[k] = u(3000, 30000); G.P[k] = u(0, 6.3); G.D[k] = u(0, 360); } }
    else G.base = u(0, 50);
    const double ascent = u(4.5, 6.0), burst = u(20000, 34000);
    const double K0 = sc.noChute ? u(14, 22) : u(4.0, 7.0);
    const double kVarA = u(-0.08, 0.08), kVarP = u(0, 6.3);   // the drag changes with height (Reynolds number, the parachute filling)
    auto Kt = [&](double h) { return K0 * (1 + kVarA * std::sin(h / 9000.0 + kVarP)); };
    const double swingR = u(1, 6), swingP = u(3, 8), swingRd = u(2, 8), swingPd = u(3, 6);
    const double gpsH = u(1.5, 4), gpsV = u(3, 6);
    const double gustS = u(0.5, 2.0), gustTau = u(10, 60);
    const double lastAgl = u(sc.lastAglMin, sc.lastAglMax);
    const double missTop = u(8000, 16000);   // missed ascent: first heard this high on the way down
    // dropouts: two gaps of 20 to 120 s somewhere
    std::vector<std::pair<double, double>> gaps;
    // truth
    double e = 0, n = 0, h = G.at(F.lat0, F.lon0) + 2, t = W.tRef - 3000 - u(0, 1200), ge = 0, gn = 0;
    bool desc = false;
    double nextFrame = t;
    const double dt = 0.2, aG = std::exp(-dt / gustTau), bG = std::sqrt(1 - aG * aG);
    double ve0 = 0, vn0 = 0, vz0 = 0;
    double tBurst = 0;
    const double tEnd0 = t;
    for (int k = 0; k < 2; k++) { const double a = u(600, 9000); gaps.push_back({tEnd0 + a, tEnd0 + a + u(20, 120)}); }
    auto llOf = [&](double ee, double nn, double& la, double& lo) { la = F.lat0 + nn / kR / kD; lo = F.lon0 + ee / (kR * std::cos(F.lat0 * kD)) / kD; };
    for (int step = 0; step < 400000; step++) {
        double la, lo; llOf(e, n, la, lo);
        const double gh = G.at(la, lo);
        if (desc && h <= gh) break;
        double we, wn; W.at(h, t, e, n, we, wn);
        const double gsc = gustS * (h - gh < 1500 ? 1.0 : 0.4);
        ge = aG * ge + bG * gsc * N(g); gn = aG * gn + bG * gsc * N(g);
        const double ve = we + ge, vn = wn + gn;
        double vz;
        if (!desc) { vz = ascent * (1 + 0.05 * std::sin(t / 300.0)); if (h >= burst) { desc = true; tBurst = t; } }
        if (desc) {
            const double vt = Kt(h) * std::sqrt(F.rho(0) / F.rho(h));
            const double since = t - tBurst;
            vz = -(since < 20 ? vt * since / 20 : vt);   // speeding up after the burst
        }
        e += ve * dt; n += vn * dt; h += vz * dt; t += dt;
        ve0 = ve; vn0 = vn; vz0 = vz;
        if (t >= nextFrame) {
            nextFrame += 1.0;
            bool lost = false;
            for (const auto& gp : gaps) if (t >= gp.first && t < gp.second) lost = true;
            if (sc.missAscent && (!desc || h > missTop)) lost = true;
            const double agl = h - gh;
            if (desc && agl < lastAgl) lost = true;
            if (U(g) < 0.03) lost = true;   // a frame with errors
            if (!lost) {
                const double ph = 2 * kPi * t / (desc ? swingPd : swingP), R = desc ? swingRd : swingR;
                const double se = R * std::cos(ph), sn = R * std::sin(ph), sve = -R * 2 * kPi / (desc ? swingPd : swingP) * std::sin(ph), svn = R * 2 * kPi / (desc ? swingPd : swingP) * std::cos(ph);
                double fe = e + se + gpsH * N(g), fn = n + sn + gpsH * N(g), fh = h + gpsV * N(g);
                if (U(g) < 0.002) { fe += u(-3000, 3000); fn += u(-3000, 3000); fh += u(-2000, 2000); }   // a GPS glitch
                Frame fr; llOf(fe, fn, fr.lat, fr.lon);
                fr.t = std::floor(t); fr.alt = fh; fr.tempC = F.tempK(h) - 273.15 + 0.3 * N(g);
                const double fve = ve0 + sve + 0.15 * N(g), fvn = vn0 + svn + 0.15 * N(g);
                fr.vz = vz0 + 0.3 * N(g); fr.hs = std::hypot(fve, fvn); fr.hd = std::atan2(fve, fvn) / kD; if (fr.hd < 0) fr.hd += 360;
                fr.trueAgl = agl; fr.desc = desc;
                F.frames.push_back(fr);
            }
        }
    }
    llOf(e, n, F.landLat, F.landLon); F.landT = t; F.landGround = G.at(F.landLat, F.landLon);
    return F;
}

// The receiver's track: one point per step, thinned to 600 points by halving (core/src/sonde/sonde_rx.cpp).
struct ThinTrack {
    std::vector<dect2::SondeTrackPoint> tr;
    double step = 1, lastT = -1e9;
    void add(const Frame& f) {
        if (f.t - lastT < 0.8 * step) return;
        dect2::SondeTrackPoint p; p.lat = (float)f.lat; p.lon = (float)f.lon; p.altM = (float)f.alt; p.unixT = (uint32_t)f.t; p.tempC = (float)f.tempC;
        tr.push_back(p); lastT = f.t;
        if (tr.size() > 600) { std::vector<dect2::SondeTrackPoint> th; for (size_t i = 0; i < tr.size(); i += 2) th.push_back(tr[i]); tr.swap(th); step *= 2; }
    }
};

inline double errM(double lat, double lon, double tLat, double tLon) {
    const double de = (lon - tLon) * kD * kR * std::cos(tLat * kD), dn = (lat - tLat) * kD * kR;
    return std::hypot(de, dn);
}

}  // namespace sim
