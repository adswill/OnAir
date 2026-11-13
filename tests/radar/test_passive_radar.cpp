// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Passive radar: clutter cancellation depth, range / Doppler accuracy, detection probability and false alarms against the simulator, tracking
// of a moving target, range ellipses, two-file source.
#include "../common/tmp_path.h"
#include "dect2/radar/passive.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dect2::radar;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static RadarConfig cfg() { RadarConfig c; c.ecaTaps = 12; return c; }

static void cpi(PassiveSimSource& s, size_t n, std::vector<cd>& ref, std::vector<cd>& surv) {
    dect2::df::Frame f; s.read(f, n);
    ref = f[0]; surv = f[1];
}

// one trial: true when a detection lies within 1.5 range bins and 3 Hz of the truth
static bool trial(double levelDb, uint64_t seed, bool fm, double* rErr = nullptr, double* fErr = nullptr, size_t* nDets = nullptr) {
    PassiveSimConfig sc; sc.seed = seed; sc.fm = fm;
    dect2::df::Rng g(seed * 7 + 3);
    SimTarget t; t.rangeM = 30e3 + 20e3 * g.uni(); t.rateMps = (g.uni() < 0.5 ? -1 : 1) * (40 + 140 * g.uni()); t.levelDb = levelDb;
    sc.targets.push_back(t);
    PassiveSimSource src(sc);
    RadarConfig rc = cfg();
    PassiveRadar rad(rc);
    std::vector<cd> ref, surv;
    cpi(src, rc.caf.cpiSamples(), ref, surv);
    const double mid = 0.5 * rc.caf.cpiSamples() / sc.sampleRate;
    RadarResult r = rad.process(ref, surv);
    if (nDets) *nDets = r.dets.size();
    const double truthR = t.rangeM + t.rateMps * mid, truthF = -t.rateMps / rc.caf.lambda();
    for (const Detection& d : r.dets)
        if (std::fabs(d.rangeM - truthR) < 1.5 * rc.caf.rangeStepM() && std::fabs(d.dopplerHz - truthF) < 3.0) {
            if (rErr) *rErr = d.rangeM - truthR;
            if (fErr) *fErr = d.dopplerHz - truthF;
            return true;
        }
    return false;
}

int main() {
    // ---- cancellation depth
    {
        PassiveSimConfig sc; sc.seed = 5;
        PassiveSimSource src(sc);
        std::vector<cd> ref, surv;
        cpi(src, 128000, ref, surv);
        std::vector<cd> a = surv, b = surv;
        const double e = cancelEca(ref, a, 12), n = cancelNlms(ref, b, 12);
        printf("cancellation: ECA %.1f dB, NLMS %.1f dB\n", e, n);
        CHECK(e > 50, "ECA depth %.1f", e);
        CHECK(n > 25, "NLMS depth %.1f", n);
    }
    // ---- detection probability and accuracy vs target level (FM and noise-like waveform)
    printf("%-8s %8s %8s %10s %10s\n", "level", "Pd FM", "Pd OFDM", "rms R (m)", "rms f (Hz)");
    const int trials = 16;
    for (double lv : {-80.0, -90.0, -100.0, -110.0}) {
        int okFm = 0, okOf = 0; double sr = 0, sf = 0; int na = 0;
        for (int k = 0; k < trials; k++) {
            double re = 0, fe = 0;
            if (trial(lv, 100 + k, true, &re, &fe)) { okFm++; sr += re * re; sf += fe * fe; na++; }
            if (trial(lv, 200 + k, false)) okOf++;
        }
        printf("%-8.0f %7.0f%% %7.0f%% %10.0f %10.2f\n", lv, 100.0 * okFm / trials, 100.0 * okOf / trials, na ? std::sqrt(sr / na) : 0, na ? std::sqrt(sf / na) : 0);
        if (lv >= -90) CHECK(okFm >= trials - 1, "Pd FM at %.0f dB: %d/%d", lv, okFm, trials);
        if (lv >= -80) { CHECK(okOf >= trials - 1, "Pd OFDM at %.0f dB: %d/%d", lv, okOf, trials); }
        if (lv == -110) CHECK(okFm <= trials / 2, "a target 110 dB down must not be reliably detected (%d/%d): the test would be too easy", okFm, trials);
        if (na) { CHECK(std::sqrt(sr / na) < 1500, "range error"); CHECK(std::sqrt(sf / na) < 1.5, "Doppler error"); }
    }
    // ---- false alarms: no target, all detections are false
    {
        size_t total = 0, cells = 0;
        for (int k = 0; k < 20; k++) {
            PassiveSimConfig sc; sc.seed = 900 + k; sc.fm = (k % 2) == 0;
            PassiveSimSource src(sc);
            RadarConfig rc = cfg();
            PassiveRadar rad(rc);
            std::vector<cd> ref, surv;
            cpi(src, rc.caf.cpiSamples(), ref, surv);
            RadarResult r = rad.process(ref, surv);
            for (const Detection& d : r.dets) printf("  FA run %d: r=%.0f m f=%.1f Hz snr=%.1f\n", k, d.rangeM, d.dopplerHz, d.snrDb);
            total += r.dets.size(); cells += (size_t)rc.caf.dopplerBins * (rc.caf.rangeBins - rc.cfar.minRangeBin);
        }
        printf("false alarms: %zu in %zu cells (Pfa set %.0e)\n", total, cells, cfg().cfar.pfa);
        CHECK(total <= 3, "too many false alarms: %zu", total);
    }
    // ---- tracking a moving target
    {
        PassiveSimConfig sc; sc.seed = 41;
        SimTarget t; t.rangeM = 42e3; t.rateMps = -150; t.levelDb = -78; sc.targets.push_back(t);
        PassiveSimSource src(sc);
        RadarConfig rc = cfg();
        PassiveRadar rad(rc);
        std::vector<cd> ref, surv;
        const double cpiS = rc.caf.cpiSamples() / sc.sampleRate;
        for (int k = 0; k < 10; k++) { cpi(src, rc.caf.cpiSamples(), ref, surv); rad.process(ref, surv); }
        int conf = 0; const Track* best = nullptr;
        for (const Track& k : rad.tracker().tracks()) if (k.confirmed) { conf++; best = &k; }
        CHECK(conf == 1, "expected one confirmed track, got %d (of %zu)", conf, rad.tracker().tracks().size());
        if (best) {
            const double truthR = t.rangeM + t.rateMps * 10 * cpiS * 0.95;   // roughly the last CPI centre
            printf("track: range %.0f m (truth ~%.0f), rate %.1f m/s (truth %.0f), hits %d\n", best->r, t.rangeM + t.rateMps * (9.5 * cpiS), best->rdot, t.rateMps, best->hits);
            (void)truthR;
            CHECK(std::fabs(best->r - (t.rangeM + t.rateMps * 9.5 * cpiS)) < 1500, "track range");
            CHECK(std::fabs(best->rdot - t.rateMps) < 15, "track rate");
            CHECK(best->hits >= 8, "hits %d", best->hits);
        }
    }
    // ---- ellipse
    {
        LatLon tx{54.90, 23.90}, rx{54.70, 24.30};
        const double L = distanceM(tx, rx), ex = 25e3;
        auto pts = bistaticEllipse(tx, rx, ex);
        CHECK(pts.size() == 120, "ellipse points");
        double worst = 0;
        for (const LatLon& p : pts) worst = std::max(worst, std::fabs(distanceM(tx, p) + distanceM(p, rx) - L - ex));
        printf("ellipse: baseline %.1f km, worst path error %.0f m\n", L / 1e3, worst);
        CHECK(worst < 300, "ellipse path error %.0f m", worst);
        CHECK(bistaticEllipse(tx, rx, -5).empty(), "negative excess");
        CHECK(std::fabs(bearingDeg({0, 0}, {1, 0})) < 1e-6, "bearing north");
    }
    // ---- two-file source round trip
    {
        const TmpPath ra = tmpPath("pr_test_ref.cf32"); const TmpPath rb = tmpPath("pr_test_surv.cf32");
        FILE* fa = fopen(ra, "wb"); FILE* fb = fopen(rb, "wb");
        for (int i = 0; i < 100; i++) { float x[2] = {(float)i, 1}, y[2] = {(float)-i, 2}; fwrite(x, 8, 1, fa); fwrite(y, 8, 1, fb); }
        fclose(fa); fclose(fb);
        TwoFileSource s(ra, rb, 250e3, 100e6);
        dect2::df::Frame f;
        CHECK(s.ok() && s.read(f, 60) && f[0].size() == 60 && f[1][5] == cd(-5, 2), "two-file read");
        CHECK(s.read(f, 60) && f[0].size() == 40, "short read at the end");
        CHECK(!s.read(f, 60), "end");
        std::remove(ra); std::remove(rb);
        TwoFileSource bad("/nonexistent/a", rb, 1, 1);
        CHECK(!bad.ok(), "missing file");
    }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
