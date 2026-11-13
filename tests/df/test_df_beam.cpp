// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Receive beamforming on a simulated 5-element circular array: white-noise gain, beamwidth, null depth (LCMV / MVDR), auto-steering to the
// MUSIC peak, calibration error recovery and the bank. Synthetic plane waves with noise only: no mutual coupling, ideal omni elements.
#include "dect2/df/beam.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace dect2::df;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const double FREQ = 433e6, LAM = kC / FREQ;

static double power(const std::vector<cd>& y) { double s = 0; for (auto& z : y) s += std::norm(z); return s / std::max<size_t>(1, y.size()); }

int main() {
    Array arr = Array::uca(5, Array::ucaRadiusForSpacing(5, LAM, 0.5));

    // delay-and-sum: unit gain at the look, white noise gain 10 log10(5) = 7 dB, narrow lobe, pattern peak at the look
    {
        Direction look{70, 0};
        auto w = delaySumWeights(arr, LAM, look);
        double g = whiteNoiseGainDb(w);
        CHECK(std::abs(g - 10 * std::log10(5.0)) < 0.01, "DS noise gain %.2f", g);
        CHECK(std::abs(beamResponse(w, arr, LAM, look) - 1) < 1e-9, "DS look response");
        auto p = beamPattern(w, arr, LAM, 0, 1.0);
        int pk = (int)(std::max_element(p.begin(), p.end()) - p.begin());
        CHECK(pk == 70, "DS pattern peak at %d", pk);
        double bw = beamwidthDeg(w, arr, LAM, look);
        printf("DS: white noise gain %.2f dB, beamwidth %.1f deg\n", g, bw);
        CHECK(bw > 20 && bw < 120, "DS beamwidth %.1f", bw);
    }

    // LCMV with a null: exact null toward the interferer (noise-free covariance), look stays at 0 dB
    {
        CMat r(5);
        for (int i = 0; i < 5; i++) r(i, i) = 1;
        auto w = lcmvWeights(r, arr, LAM, {0, 0}, {{130, 0}}, 1e-6);
        double nd = nullDepthDb(w, arr, LAM, {0, 0}, {130, 0});
        double lk = 10 * std::log10(beamResponse(w, arr, LAM, {0, 0}));
        printf("LCMV null depth %.1f dB, look %.3f dB, noise gain %.2f dB\n", nd, lk, whiteNoiseGainDb(w));
        CHECK(nd < -60, "LCMV null depth %.1f", nd);
        CHECK(std::abs(lk) < 0.01, "LCMV look");
    }

    // simulated scene: wanted at 40 deg (offset +20 kHz), strong interferer at 150 deg (offset -30 kHz), unknown element gains
    std::vector<cd> gains = {cd(1, 0), std::polar(1.1, 0.4), std::polar(0.9, -0.7), std::polar(1.05, 1.1), std::polar(0.95, -0.3)};
    auto make = [&](bool noiseSrc, bool wanted, bool interferer, uint64_t seed, int T) {
        SimConfig c; c.array = arr; c.freqHz = FREQ; c.sampleRate = 250e3; c.seed = seed; c.elementGains = gains;
        c.calibrationNoise = noiseSrc;
        if (wanted) { SimSource s; s.bearingDeg = 40; s.snrDb = 0; s.offsetHz = 20e3; s.bandwidthHz = 8e3; c.sources.push_back(s); }
        if (interferer) { SimSource s; s.bearingDeg = 150; s.snrDb = 25; s.offsetHz = -30e3; s.bandwidthHz = 8e3; c.sources.push_back(s); }
        SimMultiChannelSource src(c);
        Frame f; src.read(f, T);
        return f;
    };
    // calibration from the noise source, then the scene
    auto calF = make(true, false, false, 7, 8192);
    auto cal = estimateCalibration(covariance(calF), arr, LAM, std::nan(""));
    auto scene = make(false, true, true, 11, 16384);
    Frame sceneCal = scene;
    applyCalibration(sceneCal, cal);
    CMat r = covariance(sceneCal);

    // MUSIC auto-steer: two sources, the strongest is the interferer at 150
    {
        Options o; o.method = Method::Music; o.sources = 2; o.gridStepDeg = 0.5;
        double az = -1;
        bool ok = strongestPeak(r, arr, LAM, o, az);
        printf("strongest MUSIC peak %.1f deg\n", az);
        CHECK(ok && std::abs(angDiff(az, 150)) < 3, "peak %.1f", az);
    }

    // beams on the calibrated scene: output power of each, measured in the wanted and interferer directions
    {
        Direction look{40, 0}, intf{150, 0};
        auto wds = delaySumWeights(arr, LAM, look);
        auto wmv = mvdrWeights(r, arr, LAM, look, 1e-3);
        auto wlc = lcmvWeights(r, arr, LAM, look, {intf}, 1e-3);
        double ndDs = nullDepthDb(wds, arr, LAM, look, intf), ndMv = nullDepthDb(wmv, arr, LAM, look, intf), ndLc = nullDepthDb(wlc, arr, LAM, look, intf);
        printf("response toward the interferer: DS %.1f dB, MVDR %.1f dB, LCMV %.1f dB\n", ndDs, ndMv, ndLc);
        CHECK(ndLc < -40, "LCMV null %.1f after calibration", ndLc);
        CHECK(ndMv < ndDs - 10, "MVDR must suppress the interferer more than DS (%.1f vs %.1f)", ndMv, ndDs);
        // output interference power: the wanted-only run vs the interferer-only run through the same weights (same noise realisation per run)
        auto run = [&](bool wnt, bool itf, uint64_t seed, const std::vector<cd>& w) {
            Frame f = make(false, wnt, itf, seed, 16384); applyCalibration(f, cal); return power(beamform(f, w));
        };
        double sDs = run(true, false, 21, wds), iDs = run(false, true, 22, wds);
        double sLc = run(true, false, 21, wlc), iLc = run(false, true, 22, wlc);
        double nDs = run(false, false, 23, wds), nLc = run(false, false, 23, wlc);
        double sirDs = 10 * std::log10((sDs - nDs) / std::max(iDs - nDs, 1e-12)), sirLc = 10 * std::log10((sLc - nLc) / std::max(iLc - nLc, 1e-12));
        printf("output wanted/interferer ratio: DS %.1f dB, LCMV %.1f dB (input was -25 dB)\n", sirDs, sirLc);
        CHECK(sirLc > sirDs + 15, "LCMV improves SIR by %.1f dB", sirLc - sirDs);
    }

    // bank: two beams, one auto-steered; samples out match the block length
    {
        BeamBankConfig bc; bc.array = arr; bc.wavelength = LAM; bc.calibration = cal;
        bc.df.method = Method::Music; bc.df.sources = 2;
        BeamSpec a; a.type = BeamType::Lcmv; a.look = {40, 0}; a.nulls = {{150, 0}}; a.loading = 1e-3;
        BeamSpec b; b.type = BeamType::Mvdr; b.autoSteer = true;
        bc.beams = {a, b};
        BeamBank bank(bc);
        auto out = bank.process(scene);
        CHECK(out.size() == 2 && out[0].size() == scene[0].size(), "bank output shape");
        printf("auto-steered beam look %.1f deg\n", bank.beam(1).look.azDeg);
        CHECK(std::abs(angDiff(bank.beam(1).look.azDeg, 150)) < 3, "auto steer %.1f", bank.beam(1).look.azDeg);
        CHECK(power(out[0]) < power(out[1]), "the nulled beam carries less power than the beam on the strong interferer");
    }

    // 3D array: elevation changes the steering only with element heights
    {
        Array a3 = arr;
        a3.z = {0, 0.1, 0, 0.1, 0.2};
        auto s0 = steeringAE(a3, LAM, 30, 0), s1 = steeringAE(a3, LAM, 30, 40), p0 = steeringAE(arr, LAM, 30, 40), q0 = steering(arr, LAM, 30);
        double d3 = 0, dp = 0;
        for (int i = 0; i < 5; i++) { d3 += std::abs(s0[i] - s1[i]); dp += std::abs(p0[i] - q0[i]); }
        CHECK(d3 > 0.1, "3D steering ignores elevation");
        CHECK(dp > 0 && dp < 2.0, "planar array elevation scales the horizontal wave vector");
        auto w = delaySumWeights(a3, LAM, {30, 40});
        CHECK(std::abs(beamResponse(w, a3, LAM, {30, 40}) - 1) < 1e-9, "3D look response");
        CHECK(beamResponse(w, a3, LAM, {30, -40}) < 0.95, "3D beam distinguishes elevation");
    }

    printf(fails ? "df beam: %d failures\n" : "df beam: all passed\n", fails);
    return fails ? 1 : 0;
}
