// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ILS receiver: localizer and glide slope DDM across the needle range and through the faults of tests/common/impair.h (carrier offset +-5 kHz,
// clock +-50 ppm, low SNR, multipath echo, 8-bit clipping); the marker beacons; the localizer identification; the frequency pairing.
// Target: DDM within 0.005.
#include "dect2/ils/ils.h"
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

constexpr double kRate = 250000, kOff = 30000;

struct Result { VorTelemetry t; double worst = 999; };

static Result run(VorGenConfig g, double secs, double dialMhz, const std::function<void(std::vector<impair::cf32>&)>& fault = nullptr, double judge = 4) {
    g.rate = kRate; g.dialOffsetHz = kOff;
    VorGenerator gen(g);
    std::vector<cf32> x((size_t)(secs * kRate));
    gen.generate(x.data(), x.size());
    if (fault) fault(x);
    VorReceiver rx;
    rx.setSilent(true);
    rx.setFunction(2);
    rx.setSignalOffset(kOff);
    rx.setCenterHz(dialMhz > 0 ? dialMhz * 1e6 + vorTuning().tuneOffsetHz : 0);
    rx.configure(kRate);
    Result r;
    uint64_t seq = 0;
    const size_t step = (size_t)(0.01 * kRate);
    for (size_t i = 0; i < x.size(); i += step) {
        rx.feed(x.data() + i, std::min(step, x.size() - i));
        VorTelemetry t;
        if (rx.telemetry(t, seq)) {
            seq = t.seq; r.t = t;
            if ((double)i / kRate > secs - judge) {
                const double e = t.ilsValid ? std::fabs(t.ddm - g.ddm) : 999;
                r.worst = r.worst > 900 ? e : std::max(r.worst, e);
            }
        }
    }
    return r;
}

int main() {
    // pairing (Annex 10 Vol I 3.1.6.1)
    CHECK(std::fabs(ilsGlideslopeFor(108.10) - 334.70) < 1e-9 && std::fabs(ilsGlideslopeFor(111.95) - 330.95) < 1e-9 && std::fabs(ilsGlideslopeFor(110.30) - 335.00) < 1e-9, "pairing");
    CHECK(ilsGlideslopeFor(108.20) == 0 && !ilsIsLocalizer(113.0) && std::fabs(ilsLocalizerFor(329.15) - 108.95) < 1e-9, "not localizer channels");
    CHECK(navFunctionFor(0, 110.30e6) == NavFunction::Localizer && navFunctionFor(0, 113.0e6) == NavFunction::Vor && navFunctionFor(2, 332.0e6) == NavFunction::Glideslope
          && navFunctionFor(2, 75.0e6) == NavFunction::Marker && navFunctionFor(1, 110.3e6) == NavFunction::Vor, "functions");

    VorGenConfig loc; loc.kind = 2; loc.snrDb = 35; loc.ident = ""; loc.voice = false;
    printf("%-36s %8s %8s %7s %7s\n", "case", "ddm", "err", "m90", "m150");
    double worstAll = 0;
    auto show = [&](const char* nm, const Result& r) { printf("%-36s %+8.4f %8.5f %7.4f %7.4f %s\n", nm, r.t.ddm, r.worst, r.t.m90, r.t.m150, r.t.ident.c_str()); worstAll = std::max(worstAll, r.worst); };
    for (double d : {-0.155, -0.093, -0.031, 0.0, 0.0155, 0.062, 0.155, 0.3}) {
        VorGenConfig g = loc; g.ddm = d;
        const Result r = run(g, 6, 110.30);
        char nm[64]; snprintf(nm, sizeof nm, "localizer DDM %+.4f", d);
        show(nm, r);
        CHECK(r.worst < 0.005, "%s: error %.5f", nm, r.worst);
        CHECK(r.t.function == NavFunction::Localizer && std::fabs(r.t.sdm - 0.4) < 0.01, "%s: function %d sdm %.3f", nm, (int)r.t.function, r.t.sdm);
    }
    for (double d : {-0.175, -0.0875, 0.0, 0.0875, 0.175}) {
        VorGenConfig g = loc; g.kind = 3; g.ddm = d;
        const Result r = run(g, 6, 335.00);
        char nm[64]; snprintf(nm, sizeof nm, "glide slope DDM %+.4f", d);
        show(nm, r);
        CHECK(r.worst < 0.005, "%s: error %.5f", nm, r.worst);
        CHECK(r.t.function == NavFunction::Glideslope && std::fabs(r.t.sdm - 0.8) < 0.015, "%s: function %d sdm %.3f", nm, (int)r.t.function, r.t.sdm);
    }
    struct Case { const char* name; double snr, cfo; std::function<void(std::vector<impair::cf32>&)> f; };
    const std::vector<Case> cases = {
        {"carrier +5 kHz", 35, 5000, nullptr},
        {"carrier -5 kHz", 35, -5000, nullptr},
        {"clock +50 ppm", 35, 0, [](std::vector<impair::cf32>& x) { x = impair::clock(x, 50); }},
        {"clock -50 ppm", 35, 0, [](std::vector<impair::cf32>& x) { x = impair::clock(x, -50); }},
        {"low SNR 15 dB (25 kHz)", 15, 0, nullptr},
        {"low SNR 15 dB, -4 kHz, -40 ppm", 15, -4000, [](std::vector<impair::cf32>& x) { x = impair::clock(x, -40); }},
        {"echo 6 us -6 dB", 35, 0, [](std::vector<impair::cf32>& x) { impair::echo(x, 2, -6, 2.5); }},
        {"8-bit, peak at full scale", 35, 700, [](std::vector<impair::cf32>& x) { impair::clip8(x, 1.0 / 0.30); }},
    };
    for (int kind = 2; kind <= 3; kind++)
        for (const auto& c : cases) {
            VorGenConfig g = loc; g.kind = kind; g.ddm = kind == 2 ? 0.0775 : -0.0875; g.snrDb = c.snr; g.cfoHz = c.cfo;
            if (kind == 2) { g.ident = "IDXB"; g.identPeriod = 6.5; }
            const Result r = run(g, kind == 2 ? 16 : 6, kind == 2 ? 109.50 : 332.60, c.f);
            char nm[80]; snprintf(nm, sizeof nm, "%s %s", kind == 2 ? "LOC" : "GS ", c.name);
            show(nm, r);
            CHECK(r.worst < 0.005, "%s: error %.5f", nm, r.worst);
            if (kind == 2) CHECK(r.t.ident == "IDXB", "%s: ident '%s' (last '%s')", nm, r.t.ident.c_str(), r.t.identLast.c_str());
        }
    // overdriven 30 %: the clipped envelope loses modulation depth, the DDM shrinks with it (shown, not judged: the IQ lamp warns)
    for (int kind = 2; kind <= 3; kind++) {
        VorGenConfig g = loc; g.kind = kind; g.ddm = kind == 2 ? 0.0775 : -0.0875;
        const Result r = run(g, 6, kind == 2 ? 109.50 : 332.60, [](std::vector<impair::cf32>& x) { impair::clip8(x, 1.3 / 0.30); });
        printf("%-36s %+8.4f %8.5f %7.4f %7.4f  (overdriven, not judged)\n", kind == 2 ? "LOC 8-bit overdriven 30 %" : "GS  8-bit overdriven 30 %", r.t.ddm, r.worst, r.t.m90, r.t.m150);
    }
    // marker beacons on 75 MHz
    for (int k = 4; k <= 6; k++) {
        VorGenConfig g = loc; g.kind = k;
        const Result r = run(g, 3, 75.0);
        printf("marker %d: type %d depth %.2f\n", k - 3, r.t.marker, r.t.markerDepth);
        CHECK(r.t.function == NavFunction::Marker && r.t.marker == k - 3, "marker %d: function %d type %d", k - 3, (int)r.t.function, r.t.marker);
    }
    {   // a localizer is not a marker
        VorGenConfig g = loc;
        const Result r = run(g, 3, 110.3);
        CHECK(r.t.marker == 0, "localizer seen as marker %d", r.t.marker);
    }
    printf("worst DDM error %.5f\n", worstAll);
    printf(fails ? "ils rx: %d FAILED\n" : "ils rx: all passed\n", fails);
    return fails ? 1 : 0;
}
