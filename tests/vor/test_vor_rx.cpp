// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VOR receiver: conventional and Doppler VOR test signals at every 45 degrees and through the faults of tests/common/impair.h (carrier offset
// +-5 kHz, clock +-50 ppm, low SNR, multipath, 8-bit clipping); radial error, signal quality and the 1020 Hz identification.
// Targets: radial error < 1 degree at good SNR, < 3 degrees at low SNR, the identification decoded.
#include "dect2/vor/vor_gen.h"
#include "dect2/vor/vor_rx.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRate = 250000, kOff = -40000;

struct Result { VorTelemetry t; double worst = 999; int reports = 0; double audioRms = 0; };

static double angErr(double a, double b) { return std::fabs(std::remainder(a - b, 360.0)); }

// generate secs of the signal, apply the faults to the whole recording, feed it in 10 ms pieces; the worst error of the radial shown over
// the last `judge` seconds
static Result run(VorGenConfig g, double secs, const std::function<void(std::vector<impair::cf32>&)>& fault = nullptr, double judge = 6) {
    g.rate = kRate; g.dialOffsetHz = kOff;
    VorGenerator gen(g);
    std::vector<cf32> x((size_t)(secs * kRate));
    gen.generate(x.data(), x.size());
    if (fault) fault(x);
    VorReceiver rx;
    rx.setSilent(true);
    rx.setFunction(1);
    rx.setSignalOffset(kOff);
    rx.configure(kRate);
    double asum = 0; size_t an = 0;
    rx.setAudioTap([&](const float* a, size_t n, double) { for (size_t i = 0; i < n; i++) { asum += (double)a[i] * a[i]; an++; } });
    Result r;
    uint64_t seq = 0;
    const size_t step = (size_t)(0.01 * kRate);
    for (size_t i = 0; i < x.size(); i += step) {
        rx.feed(x.data() + i, std::min(step, x.size() - i));
        VorTelemetry t;
        if (rx.telemetry(t, seq)) {
            seq = t.seq; r.reports++; r.t = t;
            if ((double)i / kRate > secs - judge) {
                const double e = t.vorValid ? angErr(t.radialDeg, g.radialDeg) : 999;
                r.worst = r.worst > 900 ? e : std::max(r.worst, e);
            }
        }
    }
    r.audioRms = an ? std::sqrt(asum / (double)an) : 0;
    return r;
}

int main() {
    VorGenConfig base;
    base.snrDb = 35;
    base.identPeriod = 7.5;
    printf("%-34s %8s %8s %6s %6s %7s %s\n", "case", "radial", "err", "am30", "sub", "dev", "ident");
    auto show = [](const char* name, const Result& r) {
        printf("%-34s %8.2f %8.3f %6.3f %6.3f %7.1f %s\n", name, r.t.radialDeg, r.worst, r.t.am30, r.t.sub9960, r.t.fmDevHz, r.t.ident.c_str());
    };
    // every 45 degrees, conventional and Doppler
    double worstGood = 0;
    for (int kind = 0; kind < 2; kind++)
        for (int a = 0; a < 360; a += 45) {
            VorGenConfig g = base; g.kind = kind; g.radialDeg = a + 0.3 * kind + 12.7 * (a == 135);
            g.ident = "";
            const Result r = run(g, 9);
            char nm[64]; snprintf(nm, sizeof nm, "%s %5.1f", kind ? "DVOR" : "VOR ", g.radialDeg);
            show(nm, r);
            worstGood = std::max(worstGood, r.worst);
            CHECK(r.worst < 1.0, "%s radial %.1f: error %.3f", kind ? "DVOR" : "VOR", g.radialDeg, r.worst);
        }
    // the faults, each for both kinds
    struct Case { const char* name; double snr; double cfo; std::function<void(std::vector<impair::cf32>&)> f; double tol; double reflDb; double t30; double t9960; };
    const std::vector<Case> cases = {
        {"carrier +5 kHz", 35, 5000, nullptr, 1.0, -99, 30, 9960},
        {"carrier -5 kHz", 35, -5000, nullptr, 1.0, -99, 30, 9960},
        {"clock +50 ppm", 35, 0, [](std::vector<impair::cf32>& x) { x = impair::clock(x, 50); }, 1.0, -99, 30, 9960},
        {"clock -50 ppm", 35, 0, [](std::vector<impair::cf32>& x) { x = impair::clock(x, -50); }, 1.0, -99, 30, 9960},
        {"30 Hz +1 %, 9960 Hz -1 %", 35, 0, nullptr, 1.0, -99, 30.3, 9860.4},
        {"low SNR 12 dB (25 kHz)", 12, 0, nullptr, 3.0, -99, 30, 9960},
        {"low SNR 6 dB (25 kHz)", 6, 0, nullptr, 3.0, -99, 30, 9960},
        {"low SNR 12 dB, +3 kHz, +40 ppm", 12, 3000, [](std::vector<impair::cf32>& x) { x = impair::clock(x, 40); }, 3.0, -99, 30, 9960},
        {"echo 10 us -6 dB", 35, 0, [](std::vector<impair::cf32>& x) { impair::echo(x, 3, -6, 2.0); }, 1.0, -99, 30, 9960},
        {"reflection -20 dB, 40 deg, 1 Hz", 35, 0, nullptr, 1.0, -20, 30, 9960},
        {"8-bit clipping (peak 1.3)", 35, 1200, [](std::vector<impair::cf32>& x) { impair::clip8(x, 1.3 / 0.48); }, 1.0, -99, 30, 9960},
    };
    double worstLow = 0;
    for (const auto& c : cases)
        for (int kind = 0; kind < 2; kind++) {
            VorGenConfig g = base; g.kind = kind; g.radialDeg = 237.4; g.snrDb = c.snr; g.cfoHz = c.cfo; g.tone30Hz = c.t30; g.sub9960Hz = c.t9960;
            if (c.reflDb > -98) { g.reflDb = c.reflDb; g.reflRadialDeg = g.radialDeg + 40; g.reflFadeHz = 1.0; }
            const Result r = run(g, 17, c.f);
            char nm[80]; snprintf(nm, sizeof nm, "%s %s", kind ? "DVOR" : "VOR ", c.name);
            show(nm, r);
            if (c.tol > 1) worstLow = std::max(worstLow, r.worst); else worstGood = std::max(worstGood, r.worst);
            CHECK(r.worst < c.tol, "%s: error %.3f", nm, r.worst);
            if (c.snr >= 10) CHECK(r.t.ident == "DXB", "%s: ident '%s' (last '%s')", nm, r.t.ident.c_str(), r.t.identLast.c_str());
            if (c.cfo != 0) CHECK(std::fabs(r.t.cfoHz - c.cfo) < 20, "%s: carrier offset %.1f", nm, r.t.cfoHz);
        }
    // signal quality and the voice
    {
        VorGenConfig g = base; g.radialDeg = 90;
        const Result r = run(g, 17);
        CHECK(std::fabs(r.t.am30 - 0.30) < 0.02 && std::fabs(r.t.sub9960 - 0.30) < 0.03 && std::fabs(r.t.fmDevHz - 480) < 15, "quality %.3f %.3f %.1f", r.t.am30, r.t.sub9960, r.t.fmDevHz);
        CHECK(std::fabs(r.t.subcarrierHz - 9960) < 5, "subcarrier %.1f", r.t.subcarrierHz);
        CHECK(r.audioRms > 0.02, "voice audio rms %.4f", r.audioRms);
        CHECK(r.t.snrDb > 30 && r.t.snrDb < 40, "C/N %.1f", r.t.snrDb);
        CHECK(std::fabs(vorToBearing(r.t.radialDeg) - 270) < 1, "to bearing %.1f", vorToBearing(r.t.radialDeg));
        CHECK(r.t.identWpm > 5 && r.t.identWpm < 9, "wpm %.1f", r.t.identWpm);
        // a faster ILS-style identification with a digit
        VorGenConfig h = base; h.ident = "KL2"; h.identWpm = 10; h.identPeriod = 6;
        const Result s = run(h, 14);
        CHECK(s.t.ident == "KL2", "ident KL2: '%s'", s.t.ident.c_str());
    }
    // noise only: nothing measured
    {
        VorGenConfig g = base; g.amp = 0; g.snrDb = 99;
        g.rate = kRate;
        VorGenerator gen(g);
        std::vector<cf32> x((size_t)(6 * kRate));
        gen.generate(x.data(), x.size());
        impair::noise(x, 0);
        for (auto& v : x) v = cf32(0.01f * (float)((rand() % 2001) - 1000) / 1000, 0.01f * (float)((rand() % 2001) - 1000) / 1000);
        VorReceiver rx; rx.setSilent(true); rx.setFunction(1); rx.setSignalOffset(kOff); rx.configure(kRate);
        rx.feed(x.data(), x.size());
        VorTelemetry t; rx.telemetry(t, 0);
        CHECK(!t.vorValid && t.ident.empty(), "noise: valid %d ident '%s'", t.vorValid, t.ident.c_str());
    }
    printf("(the identification is judged from 10 dB C/N up; at 6 dB it is decoded once in 17 s, not yet twice)\n");
    printf("worst radial error: good %.3f deg, low SNR %.3f deg\n", worstGood, worstLow);
    printf(fails ? "vor rx: %d FAILED\n" : "vor rx: all passed\n", fails);
    return fails ? 1 : 0;
}
