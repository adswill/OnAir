// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spectrum explorer maths: frequency text, LNB conversions both ways, the floor under a tilt, bandwidth / power / symbol rate of
// synthetic spectra with noise, signal detection, sweep steps and stitching.
#include "dect2/specx/specx_measure.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::specx;
#include "../common/test_util.h"

// |H(f)|^2 of a root-raised-cosine carrier (Rs, roll-off ro) at offset f
static double rrc2(double f, double rs, double ro) {
    const double a = std::fabs(f) / rs, e0 = (1 - ro) / 2, e1 = (1 + ro) / 2;
    if (a <= e0) return 1;
    if (a >= e1) return 0;
    return 0.5 * (1 + std::cos(M_PI / ro * (a - e0)));
}

int main() {
    double hz = 0;
    CHECK(parseFreq("1.2G", hz) && std::fabs(hz - 1.2e9) < 1, "1.2G %f", hz);
    CHECK(parseFreq("433.92M", hz) && std::fabs(hz - 433.92e6) < 1, "433.92M %f", hz);
    CHECK(parseFreq("145500k", hz) && std::fabs(hz - 145.5e6) < 1, "145500k %f", hz);
    CHECK(parseFreq("100", hz) && std::fabs(hz - 100e6) < 1, "100 %f", hz);
    CHECK(parseFreq(" 2,4 GHz", hz) && std::fabs(hz - 2.4e9) < 1, "2,4 GHz %f", hz);
    CHECK(parseFreq("915000000 Hz", hz) && std::fabs(hz - 915e6) < 1, "Hz %f", hz);
    CHECK(parseFreq("10.7ghz", hz) && std::fabs(hz - 10.7e9) < 1, "ghz %f", hz);
    CHECK(!parseFreq("abc", hz) && !parseFreq("12x", hz) && !parseFreq("", hz), "garbage accepted");

    Lnb l; l.on = true; l.loHz = 9750e6; l.kind = 0;
    CHECK(std::fabs(l.radioHz(10489.5e6) - 739.5e6) < 1 && std::fabs(l.rfHz(739.5e6) - 10489.5e6) < 1, "QO-100");
    l.kind = 1; l.loHz = 125e6;
    CHECK(std::fabs(l.radioHz(7.1e6) - 132.1e6) < 1 && std::fabs(l.rfHz(132.1e6) - 7.1e6) < 1, "upconverter");
    l.kind = 2; l.loHz = 5150e6;
    CHECK(std::fabs(l.radioHz(3800e6) - 1350e6) < 1 && std::fabs(l.rfHz(1350e6) - 3800e6) < 1 && l.inverted(), "C band");
    l.on = false;
    CHECK(l.radioHz(123e6) == 123e6 && l.rfHz(123e6) == 123e6, "off");
    int np; lnbPresets(np); CHECK(np == 7, "presets %d", np);

    // a synthetic spectrum: 8192 bins over 10 MHz, a floor of -100 dB tilted by 6 dB across, noise (exponential power per bin,
    // averaged over 8), a 2 Msym/s carrier (roll-off 0.25) at -2 MHz 30 dB up, a CW tone at +3 MHz
    const int N = 8192; const double fs = 10e6, bin = fs / N;
    std::mt19937 rng(5);
    std::gamma_distribution<double> g(8.0, 1.0 / 8);
    for (double ro : {0.20, 0.25, 0.35}) {
        std::vector<float> db(N);
        const double rs = 2e6;
        for (int k = 0; k < N; k++) {
            const double f = -fs / 2 + k * bin;
            const double fl = std::pow(10.0, (-100 + 6.0 * k / N) / 10);
            double p = fl * g(rng) + std::pow(10.0, -70 / 10.0) * rrc2(f + 2e6, rs, ro);
            if (std::fabs(f - 3e6) < bin / 2) p += 1e-5;
            db[(size_t)k] = (float)(10 * std::log10(p));
        }
        SpecView v; v.db = db.data(); v.n = N; v.f0Hz = -fs / 2; v.binHz = bin; v.enbw = 1;
        const Floor F = fitFloor(v);
        CHECK(std::fabs(F.at(0) + 100) < 1.0 && std::fabs(F.at(N) + 94) < 1.0, "floor %.2f .. %.2f", F.at(0), F.at(N));
        const Measure m = measure(v, -4e6, 0);
        CHECK(m.valid && std::fabs(m.centreHz + 2e6) < 20e3, "centre %.0f", m.centreHz);
        CHECK(std::fabs(m.bw3Hz - rs) < 0.03 * rs, "ro %.2f bw3 %.0f", ro, m.bw3Hz);
        CHECK(std::fabs(m.bw26Hz - rs * (1 + ro)) < 0.05 * rs, "ro %.2f bw26 %.0f", ro, m.bw26Hz);
        CHECK(m.flatTop && m.looksDvbs, "flat top");
        CHECK(std::fabs(m.symRateBest - rs) < 0.03 * rs, "Rs %.0f", m.symRateBest);
        CHECK(std::fabs(m.rollOff - ro) < 1e-6, "roll-off %.2f for %.2f", m.rollOff, ro);
        const int j = ro < 0.22 ? 0 : ro < 0.3 ? 1 : 2;
        CHECK(std::fabs(m.symRate[j] - rs) < 0.05 * rs, "Rs(ro) %.0f", m.symRate[j]);
        CHECK(std::fabs(m.snrDb - 30 + 3 * (1 - 0.3)) < 2.5, "snr %.1f", m.snrDb);   // the floor under -2 MHz is about -97.9
        // power: -70 dB per bin over Rs / bin bins (the RRC power integrates to Rs)
        const double pw = -70 + 10 * std::log10(rs / bin);
        CHECK(std::fabs(m.powerDb - pw) < 0.5, "power %.2f want %.2f", m.powerDb, pw);
        const Measure c = measure(v, 2.9e6, 3.1e6);
        CHECK(c.valid && !c.flatTop && std::fabs(c.peakHz - 3e6) < bin && std::fabs(c.peakDb + 50) < 0.5, "tone %.0f %.1f", c.peakHz, c.peakDb);
        const auto d = detect(v);
        int car = 0, tone = 0;
        for (const auto& s : d) { if (std::fabs(s.centreHz + 2e6) < 50e3 && s.flatTop) car++; if (std::fabs(s.peakHz - 3e6) < 2 * bin) tone++; }
        CHECK(d.size() == 2 && car == 1 && tone == 1, "detect %zu (%d %d)", d.size(), car, tone);
    }
    // noise only: nothing found
    {
        std::vector<float> db(N);
        for (int k = 0; k < N; k++) db[(size_t)k] = (float)(10 * std::log10(1e-10 * g(rng)) - 4.0 * k / N);
        SpecView v; v.db = db.data(); v.n = N; v.binHz = bin;
        CHECK(detect(v).empty(), "noise: %zu found", detect(v).size());
    }
    // sweep: steps cover the range, the stitched panorama is the flat level everywhere and keeps a carrier that sits on a step edge
    {
        const auto c = sweepCentres(950e6, 2150e6, 10e6, 0.8);
        CHECK(c.size() == 150 && std::fabs(c.front() - 954e6) < 1 && std::fabs(c.back() - 2146e6) < 1, "centres %zu", c.size());
        Panorama p; p.init(950e6, 2150e6, 4096);
        const int n = 1024; const double b = 10e6 / n;
        for (double cc : c) {
            std::vector<float> db(n);
            for (int k = 0; k < n; k++) {
                const double f = cc - 5e6 + k * b;
                db[(size_t)k] = std::fabs(f - 1500e6) < 1e6 ? -60.f : -100.f;
            }
            SpecView v; v.db = db.data(); v.n = n; v.f0Hz = cc - 5e6; v.binHz = b;
            p.add(v);
        }
        CHECK(p.complete(), "panorama has holes");
        int bad = 0;
        for (size_t k = 0; k < p.db.size(); k++) {
            const double f = p.loHz + (k + 0.5) * p.binHz;
            const float want = std::fabs(f - 1500e6) < 1e6 - p.binHz ? -60.f : std::fabs(f - 1500e6) > 1e6 + p.binHz ? -100.f : p.db[k];
            if (std::fabs(p.db[k] - want) > 0.01) bad++;
        }
        CHECK(bad == 0, "stitch: %d wrong bins", bad);
        const auto d = detect(p.view());
        CHECK(d.size() == 1 && std::fabs(d[0].centreHz - 1500e6) < 0.5e6 && std::fabs(d[0].bw3Hz - 2e6) < 3 * p.binHz, "pano detect %zu", d.size());
    }
    printf(fails ? "specx measure: %d FAILED\n" : "specx measure: all passed\n", fails);
    return fails ? 1 : 0;
}
