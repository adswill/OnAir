// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Doppler orbit refinement: a Doppler curve from a perturbed orbit (plus a receiver offset and noise) is fitted from the unperturbed elements;
// the perturbation must be recovered (the predicted Doppler matches, the timing error shrinks), outliers must not matter, and the peak tracker
// must follow a synthetic carrier.
#include "dect2/geo/doppler_fit.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::string fix(std::string l) {
    l.resize(68, ' ');
    int s = 0;
    for (char c : l) { if (c >= '0' && c <= '9') s += c - '0'; else if (c == '-') s++; }
    return l + char('0' + s % 10);
}

int main() {
    Tle truth;
    CHECK(parseTle(fix("1 33591U 09005A   24275.50000000  .00000100  00000-0  70000-4 0  999"), fix("2 33591  99.1000 300.0000 0013000 200.0000 160.0000 14.12500000800000"), truth, "NOAA 19"), "parse");
    Sgp4 st;
    CHECK(st.init(truth), "init");
    Observer obs; obs.latDeg = 54.7; obs.lonDeg = 25.3; obs.altKm = 0.1;
    const double t0 = jdToUnix(truth.epochJd) + 3 * 86400;
    const auto passes = findPasses(st, obs, t0, t0 + 2 * 86400, 20);
    CHECK(!passes.empty(), "a pass exists");
    if (passes.empty()) return 1;
    const Pass pass = passes[0];
    const double f0 = 137.1e6;

    // the "real" orbit differs from the published elements
    Tle real = truth;
    real.meanAnomDeg += 0.35; real.meanMotion += 0.0012; real.raanDeg += 0.03; real.inclDeg -= 0.01;
    Sgp4 sr; CHECK(sr.init(real), "real init");
    std::mt19937 rng(7);
    dect2::normal_distribution<double> nz(0, 1);
    const double rxOffset = 600;   // Hz: the receiver's reference is off
    auto curve = [&](double noise, bool outliers) {
        std::vector<DopplerSample> v;
        int k = 0;
        for (double t = pass.aos + 5; t < pass.los - 5; t += 2, k++) {
            Look l; if (!lookAt(sr, obs, t, l)) continue;
            DopplerSample s; s.t = t; s.hz = dopplerHz(f0, l.rangeRateKmS) + rxOffset + noise * nz(rng);
            if (outliers && k % 17 == 5) s.hz += 3000;
            v.push_back(s);
        }
        return v;
    };
    auto timingErr = [&](const Tle& cand) {   // along-track timing error vs the real orbit, seconds (distance at the pass middle / speed)
        Sgp4 sc; sc.init(cand);
        const double tm = pass.tca;
        double rr[3], vv[3], r2[3], v2[3];
        sr.propagate((tm - jdToUnix(real.epochJd)) / 60, rr, vv);
        sc.propagate((tm - jdToUnix(cand.epochJd)) / 60, r2, v2);
        const double d = std::sqrt((rr[0] - r2[0]) * (rr[0] - r2[0]) + (rr[1] - r2[1]) * (rr[1] - r2[1]) + (rr[2] - r2[2]) * (rr[2] - r2[2]));
        return d / std::sqrt(vv[0] * vv[0] + vv[1] * vv[1] + vv[2] * vv[2]);
    };

    DopplerFitOptions opt;
    // clean curve
    {
        const auto v = curve(0.0, false);
        DopplerFit f;
        CHECK(fitDopplerOrbit(truth, obs, f0, v, opt, f), "clean fit: %s", f.why.c_str());
        printf("clean: rms %.1f -> %.2f Hz, offset %.1f, timing before %.3f s after %.3f s, sigma %.3f s\n", f.rmsBeforeHz, f.rmsAfterHz, f.offsetHz, timingErr(truth), timingErr(f.refined), f.alongTrackSigmaSec);
        CHECK(f.rmsAfterHz < 5, "clean rms %.2f", f.rmsAfterHz);
        CHECK(f.rmsAfterHz < f.rmsBeforeHz / 10, "rms improves %.1f -> %.2f", f.rmsBeforeHz, f.rmsAfterHz);
        CHECK(std::fabs(f.offsetHz - rxOffset) < 50, "offset %.1f", f.offsetHz);
        CHECK(timingErr(f.refined) < timingErr(truth) / 3, "timing %.3f -> %.3f", timingErr(truth), timingErr(f.refined));
    }
    // noisy curve, with outliers
    for (double noise : {10.0, 40.0}) {
        const auto v = curve(noise, true);
        DopplerFit f;
        CHECK(fitDopplerOrbit(truth, obs, f0, v, opt, f), "noisy fit: %s", f.why.c_str());
        printf("noise %.0f: rms %.1f -> %.1f Hz, rejected %d, offset %.1f, timing %.3f -> %.3f s (sigma %.3f)\n", noise, f.rmsBeforeHz, f.rmsAfterHz, f.rejected, f.offsetHz, timingErr(truth), timingErr(f.refined), f.alongTrackSigmaSec);
        CHECK(f.rmsAfterHz < noise * 1.6 + 2, "noisy rms %.1f", f.rmsAfterHz);
        CHECK(f.rejected >= 3, "outliers dropped %d", f.rejected);
        CHECK(std::fabs(f.offsetHz - rxOffset) < 120, "offset %.1f", f.offsetHz);
        CHECK(timingErr(f.refined) < timingErr(truth), "timing improves %.3f -> %.3f", timingErr(truth), timingErr(f.refined));
        CHECK(timingErr(f.refined) < 4 * f.alongTrackSigmaSec + 0.2, "reported sigma %.3f vs actual %.3f", f.alongTrackSigmaSec, timingErr(f.refined));
    }
    // a perfect orbit stays put
    {
        std::vector<DopplerSample> v;
        for (double t = pass.aos + 5; t < pass.los - 5; t += 2) { Look l; lookAt(st, obs, t, l); DopplerSample s; s.t = t; s.hz = dopplerHz(f0, l.rangeRateKmS) + 25 * nz(rng); v.push_back(s); }
        DopplerFit f;
        CHECK(fitDopplerOrbit(truth, obs, f0, v, opt, f), "null fit");
        CHECK(std::fabs(f.dMeanAnomDeg) < 0.1 && std::fabs(f.offsetHz) < 60, "null: dM %.3f offset %.1f", f.dMeanAnomDeg, f.offsetHz);
    }
    // too few / too short
    {
        DopplerFit f;
        std::vector<DopplerSample> v(5);
        CHECK(!fitDopplerOrbit(truth, obs, f0, v, opt, f), "too few rejected");
    }
    // formatting round trip
    {
        DopplerFit f;
        const auto v = curve(10, false);
        fitDopplerOrbit(truth, obs, f0, v, opt, f);
        std::string a, b; formatTle(f.refined, a, b);
        Tle back;
        CHECK(a.size() == 69 && b.size() == 69, "line lengths %zu %zu", a.size(), b.size());
        CHECK(parseTle(a, b, back), "formatted lines parse: %s | %s", a.c_str(), b.c_str());
        CHECK(std::fabs(back.meanMotion - f.refined.meanMotion) < 1e-8 && std::fabs(back.meanAnomDeg - f.refined.meanAnomDeg) < 1e-4 &&
              std::fabs(back.epochJd - f.refined.epochJd) < 2e-8 && back.norad == 33591, "round trip");
        CHECK(derivedTleText(f, f0).find("DERIVED") != std::string::npos, "marked derived");
        const auto again = parseTleText(derivedTleText(f, f0));
        CHECK(again.size() == 1 && again[0].norad == 33591 && again[0].name.find("derived") != std::string::npos, "the saved text parses back (%zu)", again.size());
        Tle orig; formatTle(truth, a, b);
        CHECK(parseTle(a, b, orig) && std::fabs(orig.bstar - truth.bstar) < 1e-9, "bstar round trip %g vs %g", orig.bstar, truth.bstar);
    }
    // the tracker
    {
        const size_t n = 4096; const double fs = 48000, centre = 137.1e6, nominal = 137.1e6;
        std::vector<float> db(n);
        std::mt19937 r2(3);
        dect2::uniform_real_distribution<float> u(-100.f, -96.f);
        DopplerTracker tr; DopplerSample s;
        bool allOk = true; double worst = 0;
        for (double off : {-3000.0, -1234.5, 0.0, 777.7, 3000.0}) {
            for (auto& x : db) x = u(r2);
            const double bin = fs / n, kf = off / bin + n / 2.0;
            const int k0 = (int)std::floor(kf); const double fr = kf - k0;
            db[k0] = -60 + float(-6 * fr); db[k0 + 1] = -60 + float(-6 * (1 - fr)); db[k0 - 1] = -75; db[k0 + 2] = -75;
            if (!tr.feed(100, db.data(), n, centre, fs, nominal, centre + off + 400, 2000, s)) allOk = false;
            else worst = std::max(worst, std::fabs(s.hz - off));
        }
        CHECK(allOk, "tracker found peaks");
        CHECK(worst < 8, "tracker error %.2f Hz", worst);
        for (auto& x : db) x = u(r2);
        CHECK(!tr.feed(100, db.data(), n, centre, fs, nominal, centre, 2000, s), "no peak in noise");
    }
    printf(fails ? "FAILED\n" : "doppler_fit OK\n");
    return fails ? 1 : 0;
}
