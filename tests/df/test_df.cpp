// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Direction finding: eigen solver, bearing accuracy vs SNR and array (RMS error table), two-source resolution, calibration recovery,
// multipath handling, beamforming, circular statistics, sources (simulator, file, Kraken header, DoA line).
#include "../common/tmp_path.h"
#include "dect2/df/df.h"
#include "dect2/df/source.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dect2::df;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const double FREQ = 433e6, LAM = kC / FREQ;

static Frame snap(SimConfig c, int T = 1024) {
    SimMultiChannelSource s(std::move(c));
    Frame f; s.read(f, T);
    return f;
}
static SimConfig cfg(const Array& a, double bearing, double snr, uint64_t seed) {
    SimConfig c; c.array = a; c.freqHz = FREQ; c.seed = seed;
    SimSource s; s.bearingDeg = bearing; s.snrDb = snr; c.sources.push_back(s);
    return c;
}
static double rmsErr(const Array& arr, Method m, double snr, int trials, int T = 1024, bool useRoot = false) {
    double ss = 0;
    for (int k = 0; k < trials; k++) {
        double b = 360.0 * (k + 0.37) / trials;
        if (arr.isUla) b = wrap360(-60 + 120.0 * (k + 0.37) / trials);   // a ULA is only good within +-60 deg of its broadside (north here)
        auto x = snap(cfg(arr, b, snr, 100 + k), T);
        CMat r = covariance(x);
        double est;
        if (useRoot) {
            auto v = rootMusic(r, arr, LAM, 1);
            // ULA front/back ambiguity: compare against the nearer of the two mirror images
            if (v.empty()) { ss += 90 * 90; continue; }
            double axis = std::atan2(arr.pos[arr.n() - 1][0] - arr.pos[0][0], arr.pos[arr.n() - 1][1] - arr.pos[0][1]) / (kPi / 180);
            double mirror = wrap360(2 * axis - v[0]);
            est = std::abs(angDiff(v[0], b)) < std::abs(angDiff(mirror, b)) ? v[0] : mirror;
        } else {
            Options o; o.method = m; o.gridStepDeg = 0.5;
            est = estimate(r, arr, LAM, o).bearingDeg;
            if (arr.isUla) { double mirror = wrap360(180 - est); if (std::abs(angDiff(mirror, b)) < std::abs(angDiff(est, b))) est = mirror; }   // front/back ambiguity
        }
        double e = angDiff(est, b);
        ss += e * e;
    }
    return std::sqrt(ss / trials);
}

int main() {
    // ---- eigen solver reconstructs a random Hermitian matrix
    {
        Rng g(5);
        int n = 5;
        CMat a(n);
        for (int i = 0; i < n; i++) for (int j = i; j < n; j++) { cd v(g.gauss(), i == j ? 0 : g.gauss()); a(i, j) = v; a(j, i) = std::conj(v); }
        std::vector<double> ev; std::vector<std::vector<cd>> vec;
        eigh(a, ev, vec);
        double err = 0;
        for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) {
            cd s = 0; for (int k = 0; k < n; k++) s += ev[k] * vec[k][i] * std::conj(vec[k][j]);
            err = std::max(err, std::abs(s - a(i, j)));
        }
        CHECK(err < 1e-9, "eigen reconstruction error %g", err);
        for (int k = 1; k < n; k++) CHECK(ev[k - 1] >= ev[k], "eigenvalues descending");
        cd d = 0; for (int i = 0; i < n; i++) d += std::conj(vec[0][i]) * vec[1][i];
        CHECK(std::abs(d) < 1e-9, "eigenvectors orthogonal");
    }
    Array uca5 = Array::uca(5, Array::ucaRadiusForSpacing(5, LAM));
    Array ula5 = Array::ula(5, LAM / 2, 90);

    // ---- accuracy table
    printf("RMS bearing error (deg), 1024 snapshots, 72 bearings (ULA: within +-60 deg of broadside), 433 MHz, half-wavelength spacing\n");
    printf("%-18s %8s %8s %8s\n", "estimator", "0 dB", "10 dB", "20 dB");
    struct Row { const char* name; const Array* a; Method m; bool root; };
    Row rows[] = {
        {"UCA-5 Bartlett", &uca5, Method::Bartlett, false}, {"UCA-5 Capon", &uca5, Method::Capon, false},
        {"UCA-5 MUSIC", &uca5, Method::Music, false}, {"UCA-5 interferom.", &uca5, Method::Interferometer, false},
        {"ULA-5 MUSIC", &ula5, Method::Music, false}, {"ULA-5 root-MUSIC", &ula5, Method::Music, true},
    };
    double tab[6][3];
    for (int r = 0; r < 6; r++) {
        double snrs[] = {0, 10, 20};
        printf("%-18s", rows[r].name);
        for (int s = 0; s < 3; s++) {
            double bsnr = snrs[s];
            // ULA only sees the half plane: bearings are the whole circle but mirror-folded in the estimator comparison; MUSIC over 360 deg picks the true one or its mirror
            tab[r][s] = rmsErr(*rows[r].a, rows[r].m, bsnr, 72, 1024, rows[r].root);
            printf(" %8.2f", tab[r][s]);
        }
        printf("\n");
    }
    // the same with a realistic uncorrected array error: random 4 deg rms phase / 0.3 dB rms amplitude errors the model does not know about
    {
        Rng g(404);
        double ss[3] = {0, 0, 0}; double snrs[] = {0, 10, 20};
        for (int s = 0; s < 3; s++) {
            int trials = 72;
            for (int k = 0; k < trials; k++) {
                SimConfig c = cfg(uca5, 360.0 * (k + 0.37) / trials, snrs[s], 500 + k);
                for (int i = 0; i < 5; i++) c.elementGains.push_back(std::polar(std::pow(10.0, 0.3 * g.gauss() / 20), 4.0 * g.gauss() * kPi / 180));
                Options o; o.gridStepDeg = 0.5;
                double e = angDiff(estimate(covariance(snap(c)), uca5, LAM, o).bearingDeg, 360.0 * (k + 0.37) / trials);
                ss[s] += e * e / trials;
            }
        }
        printf("%-18s %8.2f %8.2f %8.2f   (MUSIC, 4 deg / 0.3 dB uncorrected element errors)\n", "UCA-5 miscalibrated", std::sqrt(ss[0]), std::sqrt(ss[1]), std::sqrt(ss[2]));
        CHECK(std::sqrt(ss[2]) < 8, "miscalibrated rms %g", std::sqrt(ss[2]));
    }
    CHECK(tab[2][2] < 1.0, "UCA-5 MUSIC at 20 dB: %g", tab[2][2]);
    CHECK(tab[2][1] < 2.0, "UCA-5 MUSIC at 10 dB: %g", tab[2][1]);
    CHECK(tab[2][0] < 6.0, "UCA-5 MUSIC at 0 dB: %g", tab[2][0]);
    CHECK(tab[1][2] < 3.0, "UCA-5 Capon at 20 dB: %g", tab[1][2]);
    CHECK(tab[2][2] < tab[0][2] + 1.0, "MUSIC not worse than Bartlett at 20 dB");
    CHECK(tab[5][2] < 1.5, "ULA root-MUSIC at 20 dB: %g", tab[5][2]);
    CHECK(tab[2][0] > tab[2][2], "error falls with SNR");

    // ---- wrap at 0/360 and a few fixed bearings
    for (double b : {0.0, 359.5, 0.5, 90.0, 180.0, 270.0, 123.4}) {
        auto x = snap(cfg(uca5, b, 15, 9));
        Options o; o.gridStepDeg = 0.5;
        auto res = estimate(covariance(x), uca5, LAM, o);
        CHECK(std::abs(angDiff(res.bearingDeg, b)) < 2.0, "bearing %.1f -> %.2f", b, res.bearingDeg);
        CHECK(res.bearingDeg >= 0 && res.bearingDeg < 360, "range");
        CHECK(res.confidence > 0.5, "confidence %.2f at 15 dB", res.confidence);
    }
    // noise only: low coherence and confidence
    {
        SimConfig c = cfg(uca5, 10, -60, 3);
        auto res = estimate(covariance(snap(c)), uca5, LAM, Options());
        CHECK(res.coherence < 0.4, "noise coherence %g", res.coherence);
        CHECK(res.confidence < 0.3, "noise confidence %g", res.confidence);
    }
    // compass offset
    {
        Options o; o.compassOffsetDeg = 30;
        auto res = estimate(covariance(snap(cfg(uca5, 350, 20, 4))), uca5, LAM, o);
        CHECK(std::abs(angDiff(res.bearingDeg, 20)) < 1.5, "compass offset: %g", res.bearingDeg);
    }
    // ---- two sources, MUSIC resolves 25 deg apart at 15 dB; Bartlett cannot at 8 deg
    {
        SimConfig c = cfg(uca5, 100, 15, 21);
        SimSource s2; s2.bearingDeg = 125; s2.snrDb = 15; s2.offsetHz = 7e3; c.sources.push_back(s2);
        Options o; o.sources = 2; o.gridStepDeg = 0.5;
        auto res = estimate(covariance(snap(c, 4096)), uca5, LAM, o);
        CHECK(res.peaks.size() >= 2, "two peaks");
        if (res.peaks.size() >= 2) {
            double a = std::min(res.peaks[0].bearingDeg, res.peaks[1].bearingDeg), b = std::max(res.peaks[0].bearingDeg, res.peaks[1].bearingDeg);
            CHECK(std::abs(a - 100) < 3 && std::abs(b - 125) < 3, "two-source MUSIC: %.1f %.1f", a, b);
        }
        c.sources[1].bearingDeg = 108;
        o.method = Method::Bartlett; o.sources = 1;
        auto bres = estimate(covariance(snap(c, 4096)), uca5, LAM, o);
        int nearBy = 0; for (auto& p : bres.peaks) if (p.value > 0.5 && std::abs(angDiff(p.bearingDeg, 100)) < 20) nearBy++;
        CHECK(nearBy <= 1, "Bartlett merges 8 deg sources (%d peaks)", nearBy);
        // MDL source count
        SimConfig c3 = cfg(uca5, 100, 15, 22);
        SimSource s3; s3.bearingDeg = 220; s3.snrDb = 15; s3.offsetHz = 9e3; c3.sources.push_back(s3);
        auto r3 = covariance(snap(c3, 2048));
        std::vector<double> ev; std::vector<std::vector<cd>> v; eigh(r3, ev, v);
        CHECK(estimateSources(ev, 2048) == 2, "MDL sources: %d", estimateSources(ev, 2048));
    }
    // ---- calibration recovery: unknown gains, noise source and known-bearing methods
    {
        Rng g(77);
        std::vector<cd> gains;
        for (int i = 0; i < 5; i++) gains.push_back(std::polar(0.7 + 0.6 * g.uni(), 2 * kPi * g.uni()));
        SimConfig cn = cfg(uca5, 0, 0, 5);
        cn.elementGains = gains; cn.calibrationNoise = true; cn.calibrationSnrDb = 25;
        auto est = estimateCalibration(covariance(snap(cn, 8192)), uca5, LAM, NAN);
        // compare relative to element 0
        double err = 0, aerr = 0, am = 0;
        for (auto& z : gains) am += std::abs(z); am /= 5;
        for (int i = 0; i < 5; i++) {
            cd t = gains[i] / gains[0] * std::abs(gains[0]) / am;
            err = std::max(err, std::abs(std::arg(est[i] / t)) / (kPi / 180));
            aerr = std::max(aerr, std::abs(std::abs(est[i]) / std::abs(t) - 1));
        }
        printf("calibration (noise source): max phase error %.3f deg, max amplitude error %.3f %%\n", err, aerr * 100);
        CHECK(err < 1.0 && aerr < 0.03, "noise source calibration");
        // uncalibrated DF is wrong, calibrated is right
        SimConfig cs = cfg(uca5, 200, 20, 6); cs.elementGains = gains;
        auto xr = snap(cs);
        CMat raw = covariance(xr);
        Options o; o.gridStepDeg = 0.5;
        double bad = std::abs(angDiff(estimate(raw, uca5, LAM, o).bearingDeg, 200));
        double good = std::abs(angDiff(estimate(applyCalibration(raw, est), uca5, LAM, o).bearingDeg, 200));
        printf("uncalibrated error %.1f deg, calibrated %.2f deg\n", bad, good);
        CHECK(good < 1.5 && bad > 3, "calibration helps: %g -> %g", bad, good);
        // known-bearing calibration
        SimConfig ck = cfg(uca5, 77, 30, 7); ck.elementGains = gains;
        auto est2 = estimateCalibration(covariance(snap(ck, 4096)), uca5, LAM, 77);
        double good2 = std::abs(angDiff(estimate(applyCalibration(raw, est2), uca5, LAM, o).bearingDeg, 200));
        CHECK(good2 < 1.5, "known-bearing calibration: %g", good2);
        // raw sample path
        auto x2 = xr; applyCalibration(x2, est);
        CHECK(std::abs(angDiff(estimate(covariance(x2), uca5, LAM, o).bearingDeg, 200)) < 1.5, "raw-sample calibration");
    }
    // ---- multipath: a coherent reflection breaks plain MUSIC on a ULA, FB averaging / smoothing restores it
    {
        Array ula8 = Array::ula(8, LAM / 2, 90);
        SimConfig c = cfg(ula8, 20, 20, 31);                     // broadside is north; 20 deg and -35 deg
        SimSource refl; refl.bearingDeg = 325; refl.snrDb = 17; refl.copyOf = 0; refl.delay = 1; c.sources.push_back(refl);
        // delay of 1 sample on a narrowband (white) message decorrelates it: use delay 0 semantic via equal waveform instead
        c.sources[1].delay = 0;
        auto r = covariance(snap(c, 4096));
        Options o; o.sources = 2; o.gridStepDeg = 0.5;
        auto plain = estimate(r, ula8, LAM, o);
        o.smoothing = 5; o.forwardBackward = true;
        auto sm = estimate(r, ula8, LAM, o);
        auto has = [](const Result& res, double b) { int n = 0; for (size_t i = 0; i < res.peaks.size() && i < 4; i++) if (std::abs(angDiff(res.peaks[i].bearingDeg, b)) < 3 && res.peaks[i].value > 0.01) n = 1; return n; };
        printf("coherent multipath: plain MUSIC finds direct %d reflection %d; smoothed finds direct %d reflection %d\n", has(plain, 20), has(plain, 325), has(sm, 20), has(sm, 325));
        CHECK(has(sm, 20) && has(sm, 325), "spatial smoothing separates the coherent pair");
    }
    // ---- beamforming: listening to 60 deg with a null at 150
    {
        SimConfig c = cfg(uca5, 60, 0, 41);
        SimSource b2; b2.bearingDeg = 150; b2.snrDb = 0; b2.offsetHz = 20e3; c.sources.push_back(b2);
        auto x = snap(c, 2048);
        auto w = beamWeights(uca5, LAM, 60, {150});
        auto a60 = steering(uca5, LAM, 60), a150 = steering(uca5, LAM, 150);
        cd g60 = 0, g150 = 0;
        for (int i = 0; i < 5; i++) { g60 += std::conj(w[i]) * a60[i]; g150 += std::conj(w[i]) * a150[i]; }
        CHECK(std::abs(g60 - 1.0) < 1e-9 && std::abs(g150) < 1e-9, "beam gains %g %g", std::abs(g60), std::abs(g150));
        auto y = beamform(x, w);
        CHECK(y.size() == 2048, "beam output");
    }
    // ---- circular statistics
    {
        CHECK(std::abs(angDiff(359, 1) + 2) < 1e-12 && std::abs(angDiff(1, 359) - 2) < 1e-12, "angDiff wrap");
        CHECK(std::abs(wrap360(-10) - 350) < 1e-12 && wrap360(360) == 0 && std::abs(wrap180(190) + 170) < 1e-12, "wrap");
        std::vector<double> d = {358, 2, 359, 1, 0};
        CHECK(std::abs(angDiff(circMean(d), 0)) < 1e-9, "mean across 0: %g", circMean(d));
        CHECK(circStd(d) > 1 && circStd(d) < 3, "std %g", circStd(d));
        std::vector<double> opp = {0, 180};
        CHECK(circResultant(opp) < 1e-9, "opposite bearings resultant");
        BearingAverager av(5);
        for (double b : {359.0, 1.0, 358.0, 2.0, 0.0, 1.0}) av.add(b);
        CHECK(av.size() == 5 && std::abs(angDiff(av.mean(), 0.4)) < 1.5, "averager %g", av.mean());
        BearingAverager aw(10); aw.add(10, 1); aw.add(20, 3);
        CHECK(aw.mean() > 15, "weighted mean %g", aw.mean());
    }
    // ---- sources
    {
        // file round trip
        const TmpPath path = tmpPath("df_test_cf32.bin");
        SimMultiChannelSource sim(cfg(uca5, 40, 20, 51));
        Frame f; sim.read(f, 512);
        FILE* fp = fopen(path, "wb");
        for (size_t t = 0; t < 512; t++) for (int c = 0; c < 5; c++) { float v[2] = {(float)f[c][t].real(), (float)f[c][t].imag()}; fwrite(v, 4, 2, fp); }
        fclose(fp);
        FileMultiChannelSource fs(path, 5, 250e3, FREQ);
        Frame g;
        CHECK(fs.ok() && fs.read(g, 512) && g.size() == 5 && g[0].size() == 512, "file source");
        if (g.size() == 5 && g[0].size() == 512) CHECK(std::abs(g[3][100] - f[3][100]) < 1e-5, "file samples");
        CHECK(!fs.read(g, 512), "end of file");
        remove(path);
        // sigmf meta
        FILE* m = fopen(tmpPath("df_test.sigmf-meta"), "w");
        fputs("{\"global\":{\"core:datatype\":\"cf32_le\",\"core:sample_rate\":250000,\"core:num_channels\":5},\"captures\":[{\"core:frequency\":433000000}]}", m);
        fclose(m);
        FILE* d = fopen(tmpPath("df_test.sigmf-data"), "wb"); float z[10] = {0}; fwrite(z, 4, 10, d); fclose(d);
        FileMultiChannelSource sm(tmpPath("df_test.sigmf-meta"), 0, 0, 0);
        CHECK(sm.ok() && sm.channels() == 5 && sm.sampleRate() == 250000 && sm.centerFreq() == 433e6, "sigmf meta");
        remove(tmpPath("df_test.sigmf-meta")); remove(tmpPath("df_test.sigmf-data"));
        // Kraken header
        uint8_t hb[1024 + 5 * 4 * 8];
        memset(hb, 0, sizeof hb);
        uint32_t sw = kKrakenSyncWord; memcpy(hb, &sw, 4);
        memcpy(hb + 8, "KRAKEN-TEST", 11);
        uint32_t ants = 5, cpi = 4, bits = 32; uint64_t rf = 433000000, fsr = 2400000;
        memcpy(hb + 28, &ants, 4); memcpy(hb + 40, &rf, 8); memcpy(hb + 56, &fsr, 8); memcpy(hb + 64, &cpi, 4); memcpy(hb + 100, &bits, 4);
        float pl[5 * 4 * 2]; for (int i = 0; i < 40; i++) pl[i] = (float)i;
        memcpy(hb + 1024, pl, sizeof pl);
        KrakenIqHeader h;
        CHECK(parseKrakenIqHeader(hb, sizeof hb, h) && h.activeAntChs == 5 && h.rfCenterFreq == 433000000 && h.samplingFreq == 2400000 && h.cpiLength == 4, "kraken header");
        Frame kf;
        CHECK(decodeKrakenPayload(h, hb + 1024, sizeof pl, kf) && kf.size() == 5 && kf[1][0] == cd(8, 9), "kraken payload");
        hb[0] ^= 1;
        CHECK(!parseKrakenIqHeader(hb, sizeof hb, h), "bad sync word refused");
        // DoA lines
        auto j = parseKrakenDoaLine("{\"doa\": 271.5, \"confidence\": 87, \"freq\": 433.92, \"latitude\": 54.7, \"longitude\": 25.3}");
        CHECK(j.ok && std::abs(j.bearingDeg - 271.5) < 1e-9 && std::abs(j.confidence - 0.87) < 1e-9 && std::abs(j.freqHz - 433.92e6) < 1 && j.hasPos, "json line");
        auto cs = parseKrakenDoaLine("1699999999999, 45, 12, -60, 433920000");
        CHECK(cs.ok && cs.bearingDeg == 45 && std::abs(cs.confidence - 0.12) < 1e-9, "csv line");
        CHECK(!parseKrakenDoaLine("").ok && !parseKrakenDoaLine("{\"x\":1}").ok, "empty and unknown lines");
    }
    printf(fails ? "df: %d FAILED\n" : "df: all passed\n", fails);
    return fails ? 1 : 0;
}
