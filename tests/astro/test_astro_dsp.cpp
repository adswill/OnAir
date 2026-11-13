// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radio astronomy: the receiver integrates the synthetic sky (noise with a sloped band and a weak hydrogen line at kAstroLineKms) fed as
// fast as it can be made; the line is found at its velocity within 2 km/s with a baseline fit, and with an off-source reference instead
// (which takes out the slope by itself); frequency-switch folding on made-up spectra; the CSV export read back.
#include "dect2/astro/astro_dsp.h"
#include "dect2/astro/astro_gen.h"
#include "dect2/astro/astro_rx.h"
#include "dect2/astro/astro_tel.h"
#include "dect2/source/source.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<double> integrate(bool offSource, double fs, double sec) {
    SynthConfig sc;
    sc.modeOpt[0] = offSource ? 1 : 0;
    auto syn = makeAstroSynth(sc, fs);
    AstroReceiver rx;
    rx.configure(fs);
    rx.setFftSize(4096);
    rx.setIntegration(sec);
    std::vector<cf32> buf(1 << 16);
    const size_t total = (size_t)(fs * (sec + 0.3));   // a little past the end: the report after it
    for (size_t done = 0; done < total; done += buf.size()) { syn->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    AstroTelemetry t;
    rx.telemetry(t, 0);
    CHECK(t.completed >= 1 && t.last.size() == 4096, "integration not finished: %llu done, %zu bins", (unsigned long long)t.completed, t.last.size());
    return t.last;
}

// slope of a straight line fitted to y over the bins where use
static double slopeOf(const std::vector<double>& y, int e) {
    const int n = (int)y.size();
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = 0;
    for (int i = e; i < n - e; i++) { const double x = 2.0 * i / (n - 1) - 1; sx += x; sy += y[(size_t)i]; sxx += x * x; sxy += x * y[(size_t)i]; m++; }
    return (m * sxy - sx * sy) / (m * sxx - sx * sx);
}

int main() {
    const double fs = 2e6, sec = 30;
    {
        CHECK(std::fabs(astro::velocityKms(astro::freqForVelocity(-42, astro::kHiHz), astro::kHiHz) + 42) < 1e-9, "velocity round trip");
        CHECK(std::fabs(astro::velocityKms(astro::kHiHz + 100e3, astro::kHiHz) + 21.106) < 0.01, "100 kHz above = -21.1 km/s");
    }
    const std::vector<double> on = integrate(false, fs, sec);
    const std::vector<double> off = integrate(true, fs, sec);
    if (on.size() != 4096 || off.size() != 4096) { printf("astro dsp: %d FAILED\n", fails); return 1; }

    astro::Params p;
    p.centerHz = astro::kHiHz; p.rate = fs; p.restHz = astro::kHiHz;
    p.dcHalf = 2; p.window = true; p.winLoKms = -100; p.winHiKms = 20;
    p.smoothBins = (int)std::lround(8.0 / (fs / 4096 / astro::kHiHz * astro::kC));   // 8 km/s

    // 1. no reference, polynomial baseline
    p.degree = 3;
    const astro::Result r1 = astro::process(on, {}, p);
    printf("  baseline fit: peak %+.2f km/s (%.4f MHz), excess %.3f%%, SNR %.1f\n", r1.peakKms, r1.peakHz / 1e6, 100 * r1.peakRatio, r1.snr);
    CHECK(std::fabs(r1.peakKms - kAstroLineKms) < 2, "line at %+.2f km/s, wanted %+.1f", r1.peakKms, kAstroLineKms);
    CHECK(r1.snr > 5, "SNR %.1f", r1.snr);
    CHECK(std::fabs(r1.peakRatio - std::pow(10, kAstroLineDb / 10)) < 0.4 * std::pow(10, kAstroLineDb / 10), "excess %.4f", r1.peakRatio);

    // 2. on - off: the reference takes out the slope with no baseline fit at all
    p.degree = -1;
    const astro::Result raw = astro::process(on, {}, p);
    const astro::Result r2 = astro::process(on, off, p);
    const int e = (int)(p.edge * 4096);
    const double s0 = slopeOf(raw.ratio, e), s2 = slopeOf(r2.ratio, e);
    printf("  slope without reference %.4f, with (on - off) / off %.5f; peak %+.2f km/s, SNR %.1f\n", s0, s2, r2.peakKms, r2.snr);
    CHECK(std::fabs(s0) > 0.2, "the test band has no slope (%.4f)", s0);
    CHECK(std::fabs(s2) < 0.003, "slope left %.5f", s2);
    CHECK(std::fabs(r2.peakKms - kAstroLineKms) < 2, "on - off: line at %+.2f km/s", r2.peakKms);

    // 3. frequency switching: off = the same sky seen with the radio shifted up by 300 kHz (the line 300 kHz lower in the bins)
    {
        const int n = 4096; const double bin = fs / n, sh = 300e3;
        std::vector<double> a((size_t)n), b((size_t)n);
        const double fl = astro::freqForVelocity(-42, astro::kHiHz) - astro::kHiHz, sf = 40e3;
        for (int i = 0; i < n; i++) {
            const double f = (i - n / 2) * bin, base = 1 + 0.3 * f / (fs / 2);
            a[(size_t)i] = base * (1 + 0.01 * std::exp(-0.5 * std::pow((f - fl) / sf, 2)));
            b[(size_t)i] = base * (1 + 0.01 * std::exp(-0.5 * std::pow((f + sh - fl) / sf, 2)));
        }
        astro::Params q = p;
        q.degree = 1; q.switchHz = sh; q.fold = true; q.smoothBins = 1; q.winLoKms = -80; q.winHiKms = -5;
        const astro::Result r3 = astro::process(a, b, q);
        printf("  frequency switching: peak %+.2f km/s, excess %.4f\n", r3.peakKms, r3.peakRatio);
        CHECK(std::fabs(r3.peakKms + 42) < 0.5 && std::fabs(r3.peakRatio - 0.01) < 0.002, "folded line %+.2f km/s, %.4f", r3.peakKms, r3.peakRatio);
    }

    // 4. CSV round trip
    {
        p.degree = 3;
        const astro::Result r = astro::process(on, off, p);
        const std::string csv = astro::toCsv(r, on, p, sec);
        astro::CsvSpectrum c;
        CHECK(astro::fromCsv(csv, c), "csv parse");
        CHECK(c.freqHz.size() == on.size() && std::fabs(c.centerHz - p.centerHz) < 1e-3 && std::fabs(c.rate - fs) < 1e-3 && std::fabs(c.integrationSec - sec) < 1e-3, "csv header");
        double worst = 0;
        for (size_t i = 0; i < c.freqHz.size() && i < on.size(); i++) {
            worst = std::max(worst, std::fabs(c.freqHz[i] - r.freqHz[i]) / 1e3);
            worst = std::max(worst, std::fabs(c.velKms[i] - r.velKms[i]) / 1e-3);
            worst = std::max(worst, std::fabs(c.powerDb[i] - 10 * std::log10(on[i])) / 1e-4);
            worst = std::max(worst, std::fabs(c.ratio[i] - r.ratio[i]) / (1e-6 * std::max(1e-3, std::fabs(r.ratio[i]))));
        }
        CHECK(worst <= 1, "csv values differ (%.2f of the tolerance)", worst);
        CHECK(!astro::fromCsv("hello\n1,2\n", c), "garbage accepted");
    }

    printf(fails ? "astro dsp: %d FAILED\n" : "astro dsp: all passed\n", fails);
    return fails ? 1 : 0;
}
