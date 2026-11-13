// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Self-calibration: synthetic references at known clock errors (+-50 ppm) must come back within 0.2 ppm; the stores, the combiner, the wizard.
#include "../common/tmp_path.h"
#include "dect2/common/stable_random.h"
#include "dect2/selfcal/selfcal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace dect2::selfcal;
#include "../common/test_util.h"

static const double kPi = 3.14159265358979323846;

// a CW carrier of true frequency fnom seen by a radio centred on ctr whose clock is ppm fast
static Estimate cwRun(double fs, double ctr, double fnom, double ppm, double snrDb, double sec, unsigned seed) {
    const double fb = fnom / (1 + ppm * 1e-6) - ctr;
    ToneMeter m(fs, fnom - ctr, ctr, 100);
    std::mt19937 g(seed);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    const double ns = std::sqrt(std::pow(10.0, -snrDb / 10.0) / 2.0);   // per component, noise power in the whole band against a unit carrier
    const size_t total = (size_t)(sec * fs), chunk = 65536;
    std::vector<std::complex<float>> b(chunk);
    for (size_t n0 = 0; n0 < total; n0 += chunk) {
        const size_t c = std::min(chunk, total - n0);
        for (size_t i = 0; i < c; i++) {
            const double ph = 2 * kPi * fb * (double)(n0 + i) / fs;
            b[i] = {(float)(std::cos(ph) + ns * nd(g)), (float)(std::sin(ph) + ns * nd(g))};
        }
        m.push(b.data(), c);
    }
    return m.estimate();
}

// an FM station: MPX = mono tones + pilot + a stereo difference, 75 kHz peak deviation; the radio's clock error scales every MPX frequency
static Estimate fmRun(double fs, double ppm, double sec, unsigned seed, double off = 0, bool live = false) {
    FmPilotMeter m(fs);
    LiveCal lc(LiveCal::FmStation, 98.1e6);
    std::mt19937 g(seed);
    dect2::normal_distribution<float> nd(0.f, 0.02f);
    const size_t total = (size_t)(sec * fs), chunk = 48000;
    std::vector<std::complex<float>> b(chunk);
    double ph = 0;
    const double k = 1.0 / (1 + ppm * 1e-6);
    for (size_t n0 = 0; n0 < total; n0 += chunk) {
        const size_t c = std::min(chunk, total - n0);
        for (size_t i = 0; i < c; i++) {
            const double t = (double)(n0 + i) / fs;
            const double mpx = 0.35 * std::sin(2 * kPi * 1000 * k * t) + 0.2 * std::sin(2 * kPi * 3100 * k * t) +
                               0.1 * std::sin(2 * kPi * 19000 * k * t) + 0.15 * std::sin(2 * kPi * 38000 * k * t) * std::sin(2 * kPi * 2000 * k * t);
            ph += 2 * kPi * 75000.0 * mpx / fs;
            const double pp = ph + 2 * kPi * off * k * t;
            b[i] = {(float)(std::cos(pp) + nd(g)), (float)(std::sin(pp) + nd(g))};
        }
        if (live) lc.push(b.data(), c, fs, 98.1e6 - off); else m.push(b.data(), c);
    }
    return live ? lc.estimate() : m.estimate();
}

int main() {
    for (double p : {-50.0, -12.3, 0.0, 7.7, 50.0}) {
        const Estimate e = cwRun(2.4e6, 100e6, 100.15e6, p, 15, 3.0, 11);
        CHECK(e.valid && std::fabs(e.ppm - p) < 0.2, "cw 100 MHz p=%.1f -> %.3f (sigma %.3f valid %d)", p, e.ppm, e.sigmaPpm, (int)e.valid);
        CHECK(e.valid && e.sigmaPpm < 0.2 && e.sigmaPpm > 0, "cw sigma %.4f", e.sigmaPpm);
    }
    {
        const double f = atscPilotHz(33);
        CHECK(std::fabs(f - 584309440.559) < 1, "atsc 33 pilot %.3f", f);
        const Estimate e = cwRun(3.2e6, f - 400e3, f, -31.4, 10, 3.0, 5);
        CHECK(e.valid && std::fabs(e.ppm + 31.4) < 0.2, "atsc pilot -> %.3f", e.ppm);
    }
    {
        const Estimate e = cwRun(2.4e6, 800e6, 800.2e6, 20, -5, 4.0, 3);
        CHECK(e.valid && std::fabs(e.ppm - 20) < std::max(0.2, 4 * e.sigmaPpm), "weak -> %.3f sigma %.3f", e.ppm, e.sigmaPpm);
    }
    {
        const Estimate e = cwRun(2.4e6, 100e6, 100.15e6, 0, -60, 2.0, 9);
        CHECK(!e.valid || e.sigmaPpm > 5, "noise only gave %.3f valid %d", e.ppm, (int)e.valid);
    }
    for (double p : {-50.0, 3.2, 50.0}) {
        const Estimate e = fmRun(240000, p, 12.0, 21);
        CHECK(e.valid && std::fabs(e.ppm - p) < 0.2, "fm pilot p=%.1f -> %.3f (sigma %.3f valid %d)", p, e.ppm, e.sigmaPpm, (int)e.valid);
    }
    for (double p : {-40.0, 25.0}) {   // the live feed: a station 250 kHz off the radio's centre in a 960 ksps capture
        const Estimate e = fmRun(960000, p, 12.0, 4, 250e3, true);
        CHECK(e.valid && std::fabs(e.ppm - p) < 0.3, "live fm p=%.1f -> %.3f (sigma %.3f valid %d)", p, e.ppm, e.sigmaPpm, (int)e.valid);
    }
    {
        const Estimate e = fromFreqError("LTE cell", 1842.6e6, 1842.6e6 - 92.13, 5.0);
        CHECK(e.valid && std::fabs(e.ppm - 0.05) < 0.01 && std::fabs(e.sigmaPpm - 5.0 / 1842.6e6 * 1e6) < 1e-9, "fromFreqError %.4f", e.ppm);
        const Estimate f = fromFreqError("x", 1.575420e9, 1.575420e9 * (1 - 50e-6), 100.0);
        CHECK(std::fabs(f.ppm - 50.0025) < 0.01, "fromFreqError 50 ppm %.4f", f.ppm);
    }
    {   // a cell found 1842.6 MHz * 12 ppm low (the radio's clock is 12 ppm fast), a GPS carrier 5 kHz low, and one that is nonsense
        const Estimate c = fromCell(1842.6e6, -1842.6 * 12.0, 0);
        CHECK(c.valid && std::fabs(c.ppm - 12.0) < 0.001 && std::fabs(c.sigmaPpm - 0.05) < 1e-6, "fromCell %.4f sigma %.4f", c.ppm, c.sigmaPpm);
        const Estimate g = fromGpsCarrier(-1575.42 * 3.0);
        CHECK(g.valid && std::fabs(g.ppm - 3.0) < 0.001, "fromGpsCarrier %.4f", g.ppm);
        CHECK(!fromCell(1842.6e6, -200e3).valid, "a cfo of 108 ppm is not a clock error");
    }
    {
        std::vector<Estimate> v;
        for (double x : {10.05, 9.98, 10.02, 14.0}) { Estimate e; e.valid = true; e.ppm = x; e.sigmaPpm = 0.1; e.source = "s"; v.push_back(e); }
        Estimate bad; bad.valid = false; v.push_back(bad);
        const Estimate c = combine(v);
        CHECK(c.valid && std::fabs(c.ppm - 10.0167) < 0.01 && c.sigmaPpm < 0.07, "combine %.4f sigma %.4f", c.ppm, c.sigmaPpm);
        CHECK(!combine({}).valid && !combine({bad}).valid, "combine nothing");
    }
    {
        Store s;
        for (int i = 0; i < 10; i++) { Entry e; e.serial = "RTL0001"; e.utc = 1.8e9 + 60 * i; e.ppm = 12.0 - 0.2 * i; e.sigmaPpm = 0.1; e.tempC = 30 + i; e.source = "FM pilot"; s.add(e); }
        { Entry e; e.serial = "OTHER"; e.utc = 1.8e9; e.ppm = 3; e.sigmaPpm = 0.1; s.add(e); }
        double d = 0;
        CHECK(s.drift("RTL0001", 3600, d) && std::fabs(d + 0.2) < 1e-6, "drift %.5f", d);
        CHECK(!s.drift("OTHER", 3600, d), "drift needs points");
        double v = 0;
        CHECK(s.value("RTL0001", v) && std::fabs(v - 10.2) < 1e-6, "value while warming %.4f", v);
        const std::string path = tmpPath("selfcal_test_store.tsv");
        CHECK(s.save(path), "save");
        Store t;
        CHECK(t.load(path) && t.size() == 11, "load %zu", t.size());
        Entry l;
        CHECK(t.latest("RTL0001", l) && std::fabs(l.ppm - 10.2) < 1e-3 && l.source == "FM pilot" && std::fabs(l.tempC - 39) < 0.05, "latest");
        CHECK(!t.latest("NOPE", l), "latest none");
        remove(path.c_str());
        Store u;
        for (int i = 0; i < 6; i++) { Entry e; e.serial = "A"; e.utc = 1.8e9 + 120 * i; e.ppm = 5.0 + ((i & 1) ? 0.05 : -0.05); e.sigmaPpm = 0.1; u.add(e); }
        CHECK(u.value("A", v) && std::fabs(v - 5.0) < 0.001, "settled value %.4f", v);
        std::map<std::string, std::string> r;
        applyPpm(r, 12.3456);
        CHECK(r["ppm"] == "12.35", "applyPpm '%s'", r["ppm"].c_str());
        applyPpm(r, 0);
        CHECK(!r.count("ppm"), "applyPpm 0 removes");
    }
    {
        Wizard w(0.2, 60);
        CHECK(w.stage() == Wizard::Idle, "idle");
        w.start();
        Estimate none;
        CHECK(w.update(none, 5) == Wizard::Searching, "searching");
        Estimate e; e.valid = true; e.ppm = 4; e.sigmaPpm = 1.0;
        CHECK(w.update(e, 10) == Wizard::Integrating && w.progress() > 0 && w.progress() < 1, "integrating");
        e.sigmaPpm = 0.1;
        CHECK(w.update(e, 20) == Wizard::Done && w.progress() == 1 && std::fabs(w.result().ppm - 4) < 1e-9, "done");
        Wizard x(0.2, 30);
        x.start();
        CHECK(x.update(none, 31) == Wizard::Failed, "fails without a signal");
    }
    return testSummary("selfcal_core");
}
