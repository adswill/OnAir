// Position solver tests on pseudoranges made from the simulated constellation's geometry (noise-free and with noise, with and without the atmosphere),
// the DOP figures against a separate matrix inversion, the outlier check, and a second system with its own clock.
#include "dect2/gnss_sim.h"
#include "dect2/gnss_solve.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// observations for the satellites that the simulation transmits, at true time t
static std::vector<GnssObs> makeObs(const GnssSim& sim, double t, double bias, bool atmosphere, std::mt19937* rng, double sigma) {
    std::vector<GnssObs> obs;
    std::normal_distribution<double> nd(0.0, 1.0);
    for (size_t i = 0; i < sim.sats().size(); i++) {
        const GnssSimSat& s = sim.sats()[i];
        if (!s.transmitted) continue;
        const double tau = sim.geometricDelay(i, t);
        GnssObs o;
        o.sys = GnssGps; o.prn = s.prn;
        gpsEphemerisState(s.eph, t - tau, o.sat, nullptr);
        double ion = 0, trp = 0;
        if (atmosphere) sim.atmosphereDelays(i, t, &ion, &trp);
        o.pr = tau * kC + (ion + trp) * kC + bias;
        if (rng) o.pr += sigma * nd(*rng);
        obs.push_back(o);
    }
    return obs;
}

// horizontal and vertical standard deviation (in units of the pseudorange sigma at 90 degrees) of a weighted solution, by a separate inversion
static void weightedDop(const GnssSim& sim, const std::vector<GnssObs>& obs, double* hor, double* ver) {
    double G[4][4] = {};
    for (auto& o : obs) {
        double az, el;
        azElFromEcef(sim.receiverEcef(), o.sat, &az, &el);
        const double a = az * kPi / 180, e = el * kPi / 180;
        const double w = std::pow(std::sin(std::max(el, 3.0) * kPi / 180), 2);
        const double row[4] = {std::cos(e) * std::sin(a), std::cos(e) * std::cos(a), std::sin(e), 1.0};
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) G[i][j] += w * row[i] * row[j];
    }
    double M[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) M[i][j] = j < 4 ? G[i][j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (std::fabs(M[r][c]) > std::fabs(M[p][c])) p = r;
        for (int j = 0; j < 8; j++) std::swap(M[c][j], M[p][j]);
        const double d = M[c][c];
        for (int j = 0; j < 8; j++) M[c][j] /= d;
        for (int r = 0; r < 4; r++) if (r != c) { const double f = M[r][c]; for (int j = 0; j < 8; j++) M[r][j] -= f * M[c][j]; }
    }
    *hor = std::sqrt(M[0][4] + M[1][5]);
    *ver = std::sqrt(M[2][6]);
}

static double err3(const GnssSim& sim, const GnssSolution& s) {
    const double* r = sim.receiverEcef();
    return std::sqrt((s.x[0] - r[0]) * (s.x[0] - r[0]) + (s.x[1] - r[1]) * (s.x[1] - r[1]) + (s.x[2] - r[2]) * (s.x[2] - r[2]));
}

int main() {
    GnssSimConfig cfg;
    GnssSim sim(cfg, 4e6);
    const double t = sim.startTow() + 120.0;
    const double bias = 187654.321;
    {
        // noise-free, no atmosphere: the position must come out to a millimetre
        GnssSimConfig c2 = cfg; c2.realAtmosphere = false;
        GnssSim s2(c2, 4e6);
        auto obs = makeObs(s2, t, bias, false, nullptr, 0);
        GnssSolveOptions opt; opt.tropo = false;
        GnssSolution sl;
        CHECK(gnssSolve(obs, opt, sl), "no solution");
        CHECK(err3(s2, sl) < 2e-3, "noise-free position error %.4g m", err3(s2, sl));
        CHECK(std::fabs(sl.bias[GnssGps] - bias) < 2e-3, "clock bias %.4f, expected %.4f", sl.bias[GnssGps], bias);
        printf("noise-free: %zu satellites, position error %.2e m, clock error %.2e m, %d iterations\n", obs.size(), err3(s2, sl), sl.bias[0] - bias, sl.iterations);
    }
    {
        // with the atmosphere of the simulation, corrected by the broadcast Klobuchar model and the Saastamoinen troposphere: a few metres remain
        auto obs = makeObs(sim, t, bias, true, nullptr, 0);
        GnssSolveOptions opt;
        opt.hasIono = true; opt.iono = sim.iono(); opt.tow = t;
        GnssSolution sl, sl0;
        CHECK(gnssSolve(obs, opt, sl), "no solution");
        auto obs0 = makeObs(sim, t, bias, true, nullptr, 0);
        GnssSolveOptions opt0;
        CHECK(gnssSolve(obs0, opt0, sl0), "no solution without the atmosphere models");
        // the tropospheric model is applied inside the solver even without the ionosphere
        printf("atmosphere: error with Klobuchar %.2f m, without it %.2f m (the simulation's ionosphere is not the Klobuchar form)\n", err3(sim, sl), err3(sim, sl0));
        CHECK(err3(sim, sl) < 6.0, "position error with the atmosphere models %.2f m", err3(sim, sl));
        CHECK(err3(sim, sl) > 0.02, "the error with an imperfect atmosphere model must not be zero (%.4f)", err3(sim, sl));
        CHECK(err3(sim, sl) <= err3(sim, sl0) + 0.5, "the ionosphere model made it worse: %.2f against %.2f", err3(sim, sl), err3(sim, sl0));
    }
    {
        // DOP against a separate inversion of the geometry matrix
        auto obs = makeObs(sim, t, 0, false, nullptr, 0);
        GnssSolveOptions opt; opt.outlierCheck = false; opt.tropo = false;
        GnssSolution sl;
        CHECK(gnssSolve(obs, opt, sl), "no solution");
        const double* rx = sim.receiverEcef();
        double lat, lon, h;
        ecefToLla(rx, &lat, &lon, &h);
        const double la = lat * kPi / 180, lo = lon * kPi / 180;
        // 4x4 matrix in east, north, up, clock, built from the elevation and azimuth of each satellite
        double G[4][4] = {};
        for (auto& o : obs) {
            double az, el;
            azElFromEcef(rx, o.sat, &az, &el);   // (the satellite position is at the transmit time; the 0.07 s of earth rotation moves it by under 0.1 degree)
            const double a = az * kPi / 180, e = el * kPi / 180;
            const double row[4] = {std::cos(e) * std::sin(a), std::cos(e) * std::cos(a), std::sin(e), 1.0};
            for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) G[i][j] += row[i] * row[j];
        }
        // invert by Gauss-Jordan
        double M[4][8];
        for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) M[i][j] = j < 4 ? G[i][j] : (j - 4 == i ? 1.0 : 0.0);
        for (int c = 0; c < 4; c++) {
            int p = c;
            for (int r = c + 1; r < 4; r++) if (std::fabs(M[r][c]) > std::fabs(M[p][c])) p = r;
            for (int j = 0; j < 8; j++) std::swap(M[c][j], M[p][j]);
            const double d = M[c][c];
            for (int j = 0; j < 8; j++) M[c][j] /= d;
            for (int r = 0; r < 4; r++) if (r != c) { const double f = M[r][c]; for (int j = 0; j < 8; j++) M[r][j] -= f * M[c][j]; }
        }
        const double hd = std::sqrt(M[0][4] + M[1][5]), vd = std::sqrt(M[2][6]), pd = std::sqrt(M[0][4] + M[1][5] + M[2][6]), td = std::sqrt(M[3][7]);
        (void)la; (void)lo;
        printf("DOP: solver H %.3f V %.3f P %.3f T %.3f, separate inversion H %.3f V %.3f P %.3f T %.3f\n", sl.hdop, sl.vdop, sl.pdop, sl.tdop, hd, vd, pd, td);
        CHECK(std::fabs(sl.hdop - hd) < 0.02 * hd && std::fabs(sl.vdop - vd) < 0.02 * vd && std::fabs(sl.pdop - pd) < 0.02 * pd && std::fabs(sl.tdop - td) < 0.02 * td, "DOP differs");
    }
    {
        // a single bad pseudorange (a 150 m error) is found and left out
        std::mt19937 rng(7);
        auto obs = makeObs(sim, t, bias, false, &rng, 1.5);
        GnssSolveOptions opt; opt.tropo = false;
        GnssSolution good;
        CHECK(gnssSolve(obs, opt, good), "no solution");
        auto bad = obs;
        bad[2].pr += 150.0;
        GnssSolution sl;
        CHECK(gnssSolve(bad, opt, sl), "no solution with a bad satellite");
        CHECK(sl.excluded == 1 && !sl.used[2], "the bad satellite was not excluded (excluded %d)", sl.excluded);
        CHECK(err3(sim, sl) < 8.0, "position error with an outlier %.1f m", err3(sim, sl));
        GnssSolveOptions off; off.outlierCheck = false; off.tropo = false;
        GnssSolution sl2;
        CHECK(gnssSolve(bad, off, sl2), "no solution");
        printf("outlier: 150 m error on one of %zu satellites: error %.1f m with the check (excluded %d), %.1f m without it\n", obs.size(), err3(sim, sl), sl.excluded, err3(sim, sl2));
        CHECK(err3(sim, sl2) > 3 * err3(sim, sl), "the check did not help (%.1f against %.1f)", err3(sim, sl), err3(sim, sl2));
    }
    {
        // many noisy fixes: the scatter of the horizontal error agrees with HDOP times the pseudorange noise (weights are 1/sin^2 el, so sigma_i = s / sin(el))
        std::mt19937 rng(11);
        std::normal_distribution<double> nd(0.0, 1.0);
        double lat, lon, h;
        ecefToLla(sim.receiverEcef(), &lat, &lon, &h);
        const double la = lat * kPi / 180, lo = lon * kPi / 180;
        double sumH2 = 0, sumPred2 = 0;
        const int trials = 400;
        for (int k = 0; k < trials; k++) {
            auto obs = makeObs(sim, t, bias, false, nullptr, 0);
            for (auto& o : obs) {
                double az, el;
                azElFromEcef(sim.receiverEcef(), o.sat, &az, &el);
                o.pr += 3.0 / std::sin(std::max(el, 3.0) * kPi / 180) * nd(rng);
            }
            GnssSolveOptions opt; opt.outlierCheck = false; opt.tropo = false;
            GnssSolution sl;
            if (!gnssSolve(obs, opt, sl)) { CHECK(false, "no solution in trial %d", k); continue; }
            const double* r = sim.receiverEcef();
            const double d[3] = {sl.x[0] - r[0], sl.x[1] - r[1], sl.x[2] - r[2]};
            const double e = -std::sin(lo) * d[0] + std::cos(lo) * d[1];
            const double n = -std::sin(la) * std::cos(lo) * d[0] - std::sin(la) * std::sin(lo) * d[1] + std::cos(la) * d[2];
            sumH2 += e * e + n * n;
            double hd, vd;
            weightedDop(sim, obs, &hd, &vd);
            sumPred2 += hd * hd * 9.0;
        }
        const double rms = std::sqrt(sumH2 / trials), pred = std::sqrt(sumPred2 / trials);
        printf("scatter: measured horizontal rms %.2f m over %d fixes, predicted by the weighted geometry %.2f m\n", rms, trials, pred);
        CHECK(rms < pred * 1.12 && rms > pred * 0.88, "scatter %.2f m against %.2f m", rms, pred);
    }
    {
        // a second system with a clock of its own: the biases come out separately, the position as before
        auto obs = makeObs(sim, t, 0, false, nullptr, 0);
        const double isb = 5432.1;
        for (size_t i = 0; i < obs.size(); i += 2) { obs[i].sys = GnssGlonass; obs[i].pr += isb; }
        for (auto& o : obs) o.pr += bias;
        GnssSolveOptions opt; opt.tropo = false;
        GnssSolution sl;
        CHECK(gnssSolve(obs, opt, sl), "no two-system solution");
        CHECK(sl.sysUsed[GnssGps] && sl.sysUsed[GnssGlonass], "systems in the solution");
        CHECK(std::fabs(sl.bias[GnssGlonass] - sl.bias[GnssGps] - isb) < 5e-3, "inter-system bias %.4f, expected %.4f", sl.bias[GnssGlonass] - sl.bias[GnssGps], isb);
        CHECK(err3(sim, sl) < 8.0, "two-system position error %.2f m", err3(sim, sl));
        // a system with a single satellite cannot have a clock of its own: it is left out
        auto obs2 = makeObs(sim, t, bias, false, nullptr, 0);
        obs2[0].sys = GnssBeidou; obs2[0].pr += 999.0;
        GnssSolution s3;
        CHECK(gnssSolve(obs2, opt, s3) && !s3.sysUsed[GnssBeidou] && !s3.used[0] && s3.nUsed == (int)obs2.size() - 1, "a lone satellite of a second system should be ignored");
        printf("two systems: inter-system bias %.4f m (expected %.1f), position error %.2f m\n", sl.bias[GnssGlonass] - sl.bias[GnssGps], isb, err3(sim, sl));
    }
    {
        // too few satellites
        auto obs = makeObs(sim, t, bias, false, nullptr, 0);
        obs.resize(3);
        GnssSolveOptions opt; opt.tropo = false;
        GnssSolution sl;
        CHECK(!gnssSolve(obs, opt, sl), "three satellites cannot give a fix");
        // the starting point: a prior far away (the other side of the earth) converges to the same place
        auto o2 = makeObs(sim, t, bias, false, nullptr, 0);
        opt.hasPrior = true; opt.prior[0] = -4.0e6; opt.prior[1] = -4.7e6; opt.prior[2] = -2.7e6;
        GnssSolution s2;
        CHECK(gnssSolve(o2, opt, s2) && err3(sim, s2) < 8.0, "far prior: error %.1f m", err3(sim, s2));
    }
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
