// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ppm calibration helpers: synthetic carriers at known ppm offsets in noise, the averager's confidence, sanity limits, quick check, drift, lists.
#include "dect2/common/stable_random.h"
#include "dect2/selfcal/ppmcal.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2::selfcal;
#include "../common/test_util.h"

static const double kPi = 3.14159265358979323846;

// one second-long carrier of true frequency fnom seen by a radio centred on ctr whose clock is ppm fast; returns the measured ppm and its sigma
static bool measure(double fs, double ctr, double fnom, double ppm, double snrDb, double sec, unsigned seed, double& outPpm, double& outSigma) {
    const double fb = fnom / (1 + ppm * 1e-6) - ctr, approx = fnom - ctr;
    std::mt19937 g(seed);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    const double ns = std::sqrt(std::pow(10.0, -snrDb / 10.0) / 2.0);
    const size_t n = (size_t)(sec * fs);
    std::vector<std::complex<float>> x(n);
    for (size_t i = 0; i < n; i++) {
        const double ph = 2 * kPi * fb * (double)i / fs;
        x[i] = {(float)(std::cos(ph) + ns * nd(g)), (float)(std::sin(ph) + ns * nd(g))};
    }
    const ToneFit f = fitToneByPhase(x.data(), n, fs, approx);
    if (!f.valid) return false;
    const double meas = approx + f.offsetHz + ctr;   // absolute frequency seen
    outPpm = ppmFromObserved(fnom, meas);
    outSigma = f.sigmaHz / fnom * 1e6;
    return true;
}

int main() {
    // manual: typed pair
    CHECK(std::fabs(ppmFromObserved(100e6, 100e6 * (1 - 12e-6)) - 12.000144) < 1e-4, "manual ppm");
    CHECK(ppmFromObserved(0, 5) == 0 && ppmFromObserved(5, -1) == 0, "bad input");

    // parabolic peak: a sampled gaussian-ish peak at a fractional bin
    {
        float m[16];
        const double c = 7.3;
        for (int i = 0; i < 16; i++) m[i] = (float)std::exp(-0.5 * (i - c) * (i - c) / 1.5);
        CHECK(std::fabs(peakBin(m, 16) - c) < 0.02, "parabolic %.3f", peakBin(m, 16));
        CHECK(peakBin(nullptr, 0) == -1, "empty");
    }

    // phase-tracking estimate: ppm offsets of both signs on carriers of 19 kHz-pilot-like to 600 MHz-ATSC-like frequencies, with noise
    {
        const double cases[][3] = {{600e6 + 309440.559, 600.3e6, 25.0}, {600e6 + 309440.559, 600.3e6, -40.0}, {98.1e6, 98.1e6, 7.5}, {10e6, 10.0e6 - 50e3, -3.0}, {1.0e9, 1.0e9 - 100e3, 0.4}};
        unsigned seed = 5;
        for (const auto& c : cases) {
            double p = 0, s = 0;
            const bool ok = measure(250e3, c[1], c[0], c[2], 5.0, 4.0, seed++, p, s);
            CHECK(ok, "fit valid");
            CHECK(std::fabs(p - c[2]) < 0.1 + 4 * s, "fnom %.0f true %.2f ppm got %.3f (sigma %.4f)", c[0], c[2], p, s);
        }
    }

    // sub-Hz-per-MHz accuracy at a low SNR: -5 dB in the band for 8 s
    {
        double p = 0, s = 0;
        CHECK(measure(100e3, 0, 20e3, 10.0, -5.0, 8.0, 11, p, s), "valid at -5 dB");
        CHECK(std::fabs(p - 10.0) < 4 * s + 1.0 && s > 0, "low snr ppm %.2f sigma %.3f", p, s);
    }

    // averaging: repeated measurements of 12 ppm with noise reach a CI under 0.1 ppm, and a biased series does not claim it
    {
        std::mt19937 g(3);
        dect2::normal_distribution<float> nd(0.f, 1.f);
        PpmAverager a;
        CHECK(!a.confident() && a.count() == 0, "empty");
        int to = -1;
        for (int i = 0; i < 400 && to < 0; i++) {
            a.add(12.0 + 0.4 * nd(g), 0.4);
            if (a.confident(0.1)) to = i + 1;
        }
        CHECK(to > 0 && to < 400, "confident after %d", to);
        CHECK(std::fabs(a.mean() - 12.0) < 0.15, "mean %.3f", a.mean());
        CHECK(a.ci95() < 0.1, "ci %.3f", a.ci95());
        // claimed sigma tiny but the samples scatter: the scatter wins
        PpmAverager b;
        for (int i = 0; i < 20; i++) b.add(5.0 + ((i & 1) ? 1.0 : -1.0), 0.01);
        CHECK(b.sigma() > 0.1 && !b.confident(0.1), "scatter wins %.3f", b.sigma());
    }

    // sanity limits
    {
        CHECK(!checkPpm(350, Tcxo::No, false, 0.05).accepted, "350 rejected");
        CHECK(!checkPpm(-301, Tcxo::Yes, false, 0.05).accepted, "-301 rejected");
        CHECK(checkPpm(350, Tcxo::Unknown, false, 0.05).accepted && !checkPpm(350, Tcxo::Unknown, false, 0.05).notes.empty(), "unknown tcxo warns");
        CHECK(!checkPpm(60, Tcxo::Yes, false, 0.05).accepted && checkPpm(2, Tcxo::Yes, false, 0.05).accepted, "tcxo limit");
        const Sanity s = checkPpm(12, Tcxo::No, true, 0.05);
        CHECK(s.accepted && s.notes.size() == 1, "sub-carrier warning");
        CHECK(checkPpm(12, Tcxo::No, false, 0.05).notes.empty(), "clean case has no notes");
    }

    // quick check
    {
        CHECK(quickCheck(0.1, 0.05).verdict == QuickVerdict::Good, "good");
        CHECK(quickCheck(-1.0, 0.05).verdict == QuickVerdict::Warm, "warm");
        CHECK(quickCheck(8.0, 0.05).verdict == QuickVerdict::Off, "off");
        CHECK(quickCheck(0.5, 0.3).verdict == QuickVerdict::Good, "wide sigma tolerates");
    }

    // drift track: a radio warming up at -0.05 ppm/min over 10 minutes
    {
        DriftTrack d(600, 30);
        double slope = 0;
        CHECK(!d.slope(slope), "no slope yet");
        int n = 0;
        for (double t = 0; t <= 600; t += 5) if (d.due(t)) { d.add(t, 20.0 - 0.05 * t / 60.0, 0.03); n++; }
        CHECK(n == 21 && d.size() == 21, "points %d", n);
        CHECK(d.finished(600) && !d.finished(100), "finished");
        CHECK(d.slope(slope) && std::fabs(slope + 0.05) < 1e-6, "slope %.4f", slope);
        CHECK(d.note().find("warming") != std::string::npos, "note warming");
        DriftTrack s;
        for (int i = 0; i < 5; i++) s.add(i * 30.0, 3.0, 0.03);
        CHECK(s.note().find("settled") != std::string::npos, "note settled");
    }

    // reference lists
    {
        CHECK(fmChannels().size() == 206 && std::fabs(fmChannels()[0].nominalHz - 87.5e6) < 1, "fm list");
        CHECK(fmChannels()[0].subCarrier, "fm pilot is a sub-carrier");
        const auto at = atscChannels();
        CHECK(std::fabs(at[12].nominalHz - 473.309440559e6) > 0 && at[12].label == "ATSC ch 14 pilot" && std::fabs(at[12].nominalHz - (470e6 + 309440.559)) < 1e-3, "atsc 14");
        const auto dab = dabBlocks();
        CHECK(dab.size() == 38 && dab[0].label == "DAB 5A" && std::fabs(dab[0].nominalHz - 174.928e6) < 1, "dab list %zu", dab.size());
        bool f12b = false, f13f = false;
        for (const auto& r : dab) { if (r.label == "DAB 12B") f12b = std::fabs(r.nominalHz - 225.648e6) < 1; if (r.label == "DAB 13F") f13f = std::fabs(r.nominalHz - 239.2e6) < 1; }
        CHECK(f12b && f13f, "dab 12B / 13F");
        const auto gsm = gsmChannels();
        CHECK(gsm.size() == 124 + 374 && std::fabs(gsm[0].nominalHz - (935.2e6 + 67708.333)) < 1, "gsm");
        bool dcf = false;
        for (const auto& r : timeCarriers()) if (r.label.find("DCF77") == 0) dcf = r.nominalHz == 77500;
        CHECK(dcf, "dcf77");
        CHECK(amCarrier(1008e3).nominalHz == 1008e3 && amCarrier(1008e3).group == "AM", "am");
        CHECK(allReferences().size() > 700, "all");
    }
    return testSummary("selfcal_ppmcal");
}
