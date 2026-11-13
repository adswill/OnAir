// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Landing prediction 4.0 (dect2/sonde/sonde_predict.h), the pieces on generated data: the atmosphere, the ascent wind extraction with a
// swing, GPS noise, glitches and a dropout, the descent fit (with and without a parachute, thinned track), the Kalman drift, the
// terrain loop, the ensemble, the forecast parser on a saved Open-Meteo answer, the Tawhiri path winds, and the old API. No network.
#include "dect2/sonde/sonde_predict.h"
#include "dect2/sonde/sonde_land.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include "../common/test_util.h"
using namespace dect2;
using namespace dect2::sondepred;

namespace {
const double kD = 3.14159265358979323846 / 180, kR = 6371000.0, kLat0 = 47.0, kLon0 = 8.0;
void ll(double e, double n, double& la, double& lo) { la = kLat0 + n / kR / kD; lo = kLon0 + e / (kR * std::cos(kLat0 * kD)) / kD; }
double dist(double la, double lo, double la2, double lo2) { return std::hypot((lo - lo2) * kD * kR * std::cos(la2 * kD), (la - la2) * kD * kR); }
double wE(double h) { return 4 + 25 * std::exp(-0.5 * std::pow((h - 11000) / 2500, 2)) + 2 * std::sin(h / 1300); }
double wN(double h) { return -2 + 8 * std::exp(-0.5 * std::pow((h - 11000) / 2500, 2)) + 1.5 * std::cos(h / 900); }
std::string readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
std::string dataDir() {
#ifdef DECT2_SOURCE_DIR
    return std::string(DECT2_SOURCE_DIR) + "/tests/data/sonde/predict/";
#else
    return "tests/data/sonde/predict/";
#endif
}

// a flight in a known wind, the descent with a sea-level rate K in ISA; returns the truth landing
struct Gen {
    SondeLandTracker tr;
    std::vector<SondeTrackPoint> thin;
    double landLat = 0, landLon = 0, landT = 0;
};
Gen fly(double K, double burst, double ground, unsigned seed, bool glitches, double groundSlope = 0, double stopAgl = 0) {
    std::mt19937 g(seed); std::normal_distribution<double> N(0, 1);
    Gen G;
    double e = 0, n = 0, h = ground, t = 1.7e9;
    bool desc = false; double tb = 0;
    auto gr = [&](double ee) { return ground + groundSlope * ee; };
    int k = 0;
    while (true) {
        const double dt = 0.25;
        const double we = wE(h), wn = wN(h);
        double vz = 5.0;
        if (!desc && h >= burst) { desc = true; tb = t; }
        if (desc) { const double v = K * std::sqrt(isaDensity(0) / isaDensity(h)); vz = -(t - tb < 15 ? v * (t - tb) / 15 : v); }
        e += we * dt; n += wn * dt; h += vz * dt; t += dt; k++;
        if (desc && h <= gr(e)) break;
        if (k % 4 == 0 && !(desc && h - gr(e) < stopAgl)) {
            const double ph = t / 5.0 * 6.283;
            double la, lo; ll(e + 3 * std::cos(ph) + 3 * N(g), n + 3 * std::sin(ph) + 3 * N(g), la, lo);
            double alt = h + 4 * N(g);
            if (glitches && k % 4000 == 0) { la += 0.05; alt += 3000; }
            if (glitches && t > 1.7e9 + 1500 && t < 1.7e9 + 1560) continue;   // a dropout
            G.tr.addFix(std::floor(t), la, lo, alt, isaTempK(h) - 273.15);
            if ((k / 4) % 10 == 0) { SondeTrackPoint p; p.lat = (float)la; p.lon = (float)lo; p.altM = (float)alt; p.unixT = (uint32_t)t; p.tempC = (float)(isaTempK(h) - 273.15); G.thin.push_back(p); }
        }
    }
    ll(e, n, G.landLat, G.landLon); G.landT = t;
    return G;
}
}  // namespace

int main() {
    // ---- ISA and the atmosphere
    CHECK(std::fabs(isaDensity(0) - 1.225) < 0.002, "rho(0) %.4f", isaDensity(0));
    CHECK(std::fabs(isaDensity(11000) - 0.3639) < 0.002, "rho(11 km) %.4f", isaDensity(11000));
    CHECK(std::fabs(isaPressurePa(20000) - 5474.9) < 2, "p(20 km) %.1f", isaPressurePa(20000));
    {
        Atmosphere a; a.buildIsa();
        CHECK(std::fabs(a.density(15000) / a.rho0() - isaDensity(15000) / isaDensity(0)) < 0.003, "ISA table ratio");
        std::vector<double> b, tc;
        for (double z = 0; z < 20000; z += 250) { b.push_back(z); tc.push_back(isaTempK(z + 125) - 273.15 + 15); }
        Atmosphere w; w.build(b, tc, 250);
        CHECK(w.fromSonde, "built from temperatures");
        CHECK(w.density(10000) / w.rho0() > isaDensity(10000) / isaDensity(0) * 1.02, "warm air: thinner aloft relative to the ground? ratio %.4f vs %.4f",
              w.density(10000) / w.rho0(), isaDensity(10000) / isaDensity(0));
    }
    // ---- the geoid table (EGM96: 26.1 m at 54 N 25 E, -33.1 m at 25 N 55 E, 17.2 m at 0 N 0 E)
    CHECK(std::fabs(geoidUndulationM(54, 25) - 26.1) < 2, "geoid 54 25: %.1f", geoidUndulationM(54, 25));
    CHECK(std::fabs(geoidUndulationM(25, 55) + 33.1) < 3, "geoid 25 55: %.1f", geoidUndulationM(25, 55));
    CHECK(std::fabs(geoidUndulationM(0, 0) - 17.2) < 2, "geoid 0 0: %.1f", geoidUndulationM(0, 0));
    CHECK(std::fabs(geoidUndulationM(10, 179.9) - geoidUndulationM(10, -180.1)) < 0.01, "wraps at 180");
    // ---- ascent winds from a track with a swing, noise, glitches and a dropout
    {
        Gen G = fly(5.0, 25000, 300, 1, true);
        CHECK(G.tr.rejected() >= 2, "glitches rejected: %d", G.tr.rejected());
        const LayerWinds w = G.tr.ascentWinds(200);
        double se = 0; int n = 0, big = 0;
        for (size_t i = 0; i < w.layers.size(); i++) {
            if (!w.layers[i].ok) continue;
            const double hc = (w.firstLayer + i + 0.5) * w.layerM;
            if (hc < 600 || hc > 24000) continue;
            const double d = std::hypot(w.layers[i].ve - wE(hc), w.layers[i].vn - wN(hc));
            se += d * d; n++; if (d > 2.0) big++;
        }
        const double rms = std::sqrt(se / std::max(1, n));
        printf("ascent winds: %d layers, rms error %.2f m/s, %d over 2 m/s\n", n, rms, big);
        CHECK(n > 100, "layers %d", n);
        CHECK(rms < 0.8, "rms %.2f", rms);
        CHECK(big <= 3, "big errors %d", big);
        WindLayer x;
        CHECK(w.at(11000, x) && std::fabs(x.ve - wE(11000)) < 1.5, "at(11 km) %.1f vs %.1f", x.ve, wE(11000));
        // the thinned receiver track (one point in 10 frames): still usable
        SondeLandTracker t2; t2.seed(G.thin);
        const LayerWinds w2 = t2.ascentWinds(200);
        double se2 = 0; int n2 = 0;
        for (size_t i = 0; i < w2.layers.size(); i++) if (w2.layers[i].ok) {
            const double hc = (w2.firstLayer + i + 0.5) * w2.layerM;
            if (hc < 600 || hc > 24000) continue;
            se2 += std::pow(std::hypot(w2.layers[i].ve - wE(hc), w2.layers[i].vn - wN(hc)), 2); n2++;
        }
        printf("ascent winds from a 10 s track: %d layers, rms %.2f m/s\n", n2, std::sqrt(se2 / std::max(1, n2)));
        CHECK(n2 > 80 && std::sqrt(se2 / std::max(1, n2)) < 1.5, "thin track winds");
    }
    // ---- the descent fit
    for (double K : {5.2, 18.0}) {
        Gen G = fly(K, 22000, 200, 2, false, 0, 2000);
        Atmosphere a; a.buildIsa();
        const DescentFit f = fitDescent(G.tr.fixes(), G.tr.burstIndex(), a, 300);
        printf("descent fit: K %.2f (true %.2f), sigma %.3f, n %d, slope %.3f/km\n", f.K, K, f.sigmaRel, f.n, f.slopePerKm);
        CHECK(f.ok && std::fabs(f.K / K - 1) < 0.02, "K %.2f vs %.2f", f.K, K);
        CHECK(f.noParachute == (K > 11), "no parachute flag");
        SondeLandTracker t2; t2.seed(G.thin);
        const DescentFit f2 = fitDescent(t2.fixes(), t2.burstIndex(), a, 300);
        CHECK(f2.ok && std::fabs(f2.K / K - 1) < 0.03, "thin track K %.2f", f2.K);
    }
    // ---- the Kalman drift: steady wind, a 3 m swing, 3 m GPS noise
    {
        Gen G = fly(5.0, 15000, 0, 3, false, 0, 3000);
        const Drift d = kalmanDrift(G.tr.fixes(), G.tr.burstIndex(), 90, 4);
        const double h = G.tr.fixes().back().alt;
        printf("Kalman drift at %.0f m: %.2f %.2f (wind %.2f %.2f), sigma %.2f\n", h, d.ve, d.vn, wE(h), wN(h), d.sigmaV);
        CHECK(d.ok && std::hypot(d.ve - wE(h), d.vn - wN(h)) < 1.2, "drift error %.2f", std::hypot(d.ve - wE(h), d.vn - wN(h)));
        CHECK(d.vz < -4 && d.vz > -12, "vz %.1f", d.vz);
    }
    // ---- the whole prediction with the terrain loop: ground rising 10 m per km to the east
    {
        Gen G = fly(5.0, 20000, 100, 4, false, 0.01, 1500);
        SondePredictOpts o; o.members = 0;
        SondePredictEnv env;
        env.ground = [](double, double lo, double& h) { h = 100 + 0.01 * (lo - kLon0) * kD * kR * std::cos(kLat0 * kD); return true; };
        const SondePrediction p = G.tr.predict(o, env);
        const double err = dist(p.lat, p.lon, G.landLat, G.landLon);
        printf("terrain loop: ground %.0f m (from terrain %d), error %.0f m, %.0f s left\n", p.groundM, p.groundFromTerrain, err, p.secondsLeft);
        CHECK(p.valid && p.groundFromTerrain, "terrain used");
        CHECK(err < 150, "error with terrain %.0f m", err);
        SondePredictOpts flat = o; flat.useTerrain = false; flat.groundM = 100;
        const SondePrediction q = G.tr.predict(flat, env);
        CHECK(dist(q.lat, q.lon, G.landLat, G.landLon) > err, "flat ground is worse: %.0f vs %.0f", dist(q.lat, q.lon, G.landLat, G.landLon), err);
        // the ensemble
        SondePredictOpts e = o; e.members = 200;
        const SondePrediction r = G.tr.predict(e, env);
        printf("ensemble: r50 %.0f m, r95 %.0f m, ellipse %.0f x %.0f m at %.0f deg, time sigma %.0f s\n", r.r50M, r.r95M, r.ellMajorM, r.ellMinorM, r.ellAngleDeg, r.secondsSigma);
        CHECK(r.hasSpread && r.r50M > 0 && r.r95M > r.r50M && r.members.size() == 200, "spread");
        CHECK(std::fabs(r.lat - p.lat) < 1e-9 && std::fabs(r.lon - p.lon) < 1e-9, "the nominal point does not depend on the ensemble");
        const SondePrediction r2 = G.tr.predict(e, env);
        CHECK(r2.r95M == r.r95M, "deterministic with the same seed");
        SondePredictOpts e2 = e; e2.windChangeMsPerH = 4; e2.driftCorrLenM = 150;
        const SondePrediction r3 = G.tr.predict(e2, env);
        CHECK(r3.r95M > r.r95M, "more wind error, wider: %.0f vs %.0f", r3.r95M, r.r95M);
        CHECK(!r.text.empty(), "text");
    }
    // ---- the forecast: the saved Open-Meteo answer (2 points, 3 hours, 28 levels)
    {
        const std::string js = readFile(dataDir() + "openmeteo_gfs_sample.json");
        CHECK(!js.empty(), "sample file at %s", (dataDir() + "openmeteo_gfs_sample.json").c_str());
        ForecastWinds F; std::string err;
        CHECK(parseOpenMeteo(js, F, err), "parse: %s", err.c_str());
        CHECK(F.cols.size() == 2, "columns %zu", F.cols.size());
        if (F.cols.size() == 2) {
            CHECK(F.cols[0].times.size() == 3 && F.cols[0].levels[0].size() == 28, "times %zu levels %zu", F.cols[0].times.size(), F.cols[0].levels[0].size());
            // 250 hPa at the first point, first hour: 33.41 m/s from 274 degrees at 10140.95 m
            double ve, vn;
            const auto& c = F.cols[0];
            CHECK(F.at(c.lat, c.lon, 10140.95, c.times[0], ve, vn), "at");
            CHECK(std::fabs(ve - 33.33) < 0.3 && std::fabs(vn + 2.33) < 0.3, "250 hPa wind %.2f %.2f", ve, vn);
            CHECK(F.topM() > 29000, "top %.0f", F.topM());
            // half way between two hours: half way between the winds
            double v1, n1, v2, n2, vm, nm;
            F.at(c.lat, c.lon, 5000, c.times[0], v1, n1); F.at(c.lat, c.lon, 5000, c.times[1], v2, n2);
            F.at(c.lat, c.lon, 5000, 0.5 * (c.times[0] + c.times[1]), vm, nm);
            CHECK(std::fabs(vm - 0.5 * (v1 + v2)) < 0.3, "time interpolation");
        }
        CHECK(!parseOpenMeteo("{\"error\":true,\"reason\":\"The service is overloaded\"}", F, err) && err.find("overloaded") != std::string::npos, "error answer: %s", err.c_str());
        CHECK(!parseOpenMeteo("not json", F, err), "garbage");
        const std::string u = openMeteoUrl(forecastPoints(53.71, 7.38, 53.48, 9.49), 1794463200);
        CHECK(u.find("/v1/gfs?latitude=53.75,53.50,53.50&longitude=7.50,8.50,9.50") != std::string::npos, "url %s", u.c_str());
        CHECK(u.find("geopotential_height_10hPa") != std::string::npos && u.find("start_hour=2026-11-12T05:00") != std::string::npos, "url levels and hours %s", u.c_str());
        CHECK(SondeWindFetcher::cacheKey(forecastPoints(53.71, 7.38, 53.48, 9.49), 1794463200) == SondeWindFetcher::cacheKey(forecastPoints(53.70, 7.40, 53.49, 9.51), 1794465000),
              "the cache key is stable for a moving sonde in the same slot");
    }
    // ---- a missed ascent: only the descent from 12 km, with and without a forecast of the true wind (1.5 m/s error)
    {
        Gen G = fly(5.0, 25000, 0, 5, false, 0, 1500);
        SondeLandTracker t;
        const double tBurst = G.tr.fixes()[G.tr.burstIndex()].t;
        for (const auto& f : G.tr.fixes()) if (f.t > tBurst && f.alt < 12000) t.addFix(f.t, f.lat, f.lon, f.alt, f.tempC);
        ForecastWinds F; F.source = "test";
        ForecastWinds::Column c; c.lat = kLat0; c.lon = kLon0; c.times = {1.6e9, 1.8e9};
        std::vector<ForecastWinds::Level> L;
        for (double z = 0; z <= 30000; z += 500) L.push_back({z, wE(z) + 1.5 * std::sin(z / 3000), wN(z) - 1.5 * std::cos(z / 4000)});
        c.levels = {L, L}; F.cols.push_back(c);
        SondePredictOpts o; o.members = 0; o.groundM = 0; o.useTerrain = false;
        SondePredictEnv none, withF; withF.forecast = &F;
        const SondePrediction a = t.predict(o, none), b = t.predict(o, withF);
        const double ea = dist(a.lat, a.lon, G.landLat, G.landLon), eb = dist(b.lat, b.lon, G.landLat, G.landLon);
        printf("missed ascent from %.0f m: drift only %.0f m, with forecast %.0f m\n", t.fixes().back().alt, ea, eb);
        CHECK(b.usedForecast && eb < ea, "the forecast helps");
        // the Tawhiri path as winds: a path drifting 10 m/s east while falling
        std::vector<PathPoint> p;
        for (int i = 0; i < 50; i++) p.push_back({kLat0, kLon0 + (10.0 * 60 * i) / (kR * std::cos(kLat0 * kD)) / kD, 20000.0 - 400 * i, 1.7e9 + 60.0 * i});
        p.insert(p.begin(), PathPoint{kLat0, kLon0 - 0.01, 5000, 1.7e9 - 3000});
        const ForecastWinds T = windsFromPath(p);
        double ve = 0, vn = 0;
        CHECK(!T.empty() && T.at(kLat0, kLon0, 10000, 1.7e9, ve, vn) && std::fabs(ve - 10) < 0.2 && std::fabs(vn) < 0.2, "path winds %.2f %.2f", ve, vn);
    }
    // ---- the old API is unchanged
    {
        const SondeLanding l = predictSondeLanding(54, 25, 5000, -8, 10, 90);
        CHECK(l.valid && l.lon > 25 && std::fabs(l.lat - 54) < 0.01, "old prediction");
        std::vector<SondeTrackPoint> none;
        CHECK(predictSondeLanding40(none, SondePredictOpts(), SondePredictEnv()).valid == false, "no track, no prediction");
    }
    return testSummary("sonde_predict");
}
