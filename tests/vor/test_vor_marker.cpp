// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ILS marker beacons (75 MHz, ICAO Annex 10 Vol I 3.1.7): outer 400 Hz keyed in dashes at 2 per second, middle 1300 Hz dot-dash, inner 3000 Hz dots at 6
// per second, 95 % AM. The test signals of vor_gen.cpp through carrier offset, noise, a clock error and 8-bit clipping: the tone (marker kind), the keying
// pattern and that the keying matches the tone; negatives: a localizer signal and noise alone on 75 MHz give no marker. Simulated signals only.
#include "dect2/vor/vor_gen.h"
#include "dect2/vor/vor_rx.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRate = 250000, kOff = -40000;

static VorTelemetry run(VorGenConfig g, double secs, const std::function<void(std::vector<impair::cf32>&)>& fault = nullptr) {
    g.rate = kRate; g.dialOffsetHz = kOff; g.ident.clear();
    VorGenerator gen(g);
    std::vector<cf32> x((size_t)(secs * kRate));
    gen.generate(x.data(), x.size());
    if (fault) fault(x);
    VorReceiver rx;
    rx.setSilent(true);
    rx.setFunction(0);
    rx.setCenterHz(75e6 + vorTuning().tuneOffsetHz);   // the dial on 75.000 MHz: the marker function
    rx.setSignalOffset(kOff);
    rx.configure(kRate);
    VorTelemetry last;
    uint64_t seq = 0;
    const size_t step = (size_t)(0.01 * kRate);
    for (size_t i = 0; i < x.size(); i += step) {
        rx.feed(x.data() + i, std::min(step, x.size() - i));
        VorTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; last = t; }
    }
    return last;
}

int main() {
    CHECK(navFunctionFor(0, 75e6) == NavFunction::Marker && navFunctionFor(2, 75e6) == NavFunction::Marker, "75 MHz is the marker function");
    static const char* nm[] = {"", "outer", "middle", "inner"};
    // every marker through offsets, noise and a clock error
    for (int kind = 4; kind <= 6; kind++)
        for (double cfo : {0.0, 2500.0, -5000.0})
            for (double snr : {40.0, 20.0, 12.0})
                for (double ppm : {0.0, 50.0}) {
                    VorGenConfig g; g.kind = kind; g.cfoHz = cfo; g.snrDb = snr; g.seed = (uint32_t)(kind * 100 + snr);
                    const VorTelemetry t = run(g, 6, ppm != 0 ? std::function<void(std::vector<impair::cf32>&)>([&](std::vector<impair::cf32>& x) { x = impair::clock(x, ppm); }) : nullptr);
                    const int want = kind - 3;
                    CHECK(t.function == NavFunction::Marker, "function %d", (int)t.function);
                    CHECK(t.marker == want, "%s cfo %.0f snr %.0f ppm %.0f: marker %d", nm[want], cfo, snr, ppm, t.marker);
                    CHECK(t.markerKeying == want && t.markerKeyingOk, "%s cfo %.0f snr %.0f ppm %.0f: keying %d ok %d", nm[want], cfo, snr, ppm, t.markerKeying, (int)t.markerKeyingOk);
                    CHECK(t.markerDepth > 0.6f && t.markerDepth < 1.3f, "%s depth %.2f", nm[want], (double)t.markerDepth);
                }
    // 8-bit clipping of a strong signal
    for (int kind = 4; kind <= 6; kind++) {
        VorGenConfig g; g.kind = kind; g.snrDb = 30;
        const VorTelemetry t = run(g, 6, [](std::vector<impair::cf32>& x) { impair::clip8(x, 4.0); });
        CHECK(t.marker == kind - 3 && t.markerKeyingOk, "%s clipped: marker %d keying %d", nm[kind - 3], t.marker, t.markerKeying);
    }
    // negatives on 75 MHz: a localizer's 90 / 150 Hz and noise alone are no marker
    {
        VorGenConfig g; g.kind = 2; g.ddm = 0.05; g.snrDb = 30;
        const VorTelemetry t = run(g, 5);
        CHECK(t.marker == 0 && t.markerKeying == 0, "localizer tones read as marker %d keying %d", t.marker, t.markerKeying);
    }
    {
        VorGenConfig g; g.kind = 4; g.snrDb = 20;
        const VorTelemetry t = run(g, 5, [](std::vector<impair::cf32>& x) {   // the signal replaced by complex noise of the same level
            std::mt19937 r(7); std::normal_distribution<float> n(0.f, 0.15f);
            for (auto& v : x) v = impair::cf32(n(r), n(r));
        });
        CHECK(t.marker == 0, "noise alone read as marker %d", t.marker);
    }
    // the description line names the marker and its keying
    {
        VorGenConfig g; g.kind = 6; g.snrDb = 30;
        const VorTelemetry t = run(g, 6);
        const std::string s = vorSummary(t);
        CHECK(s.find("inner marker") != std::string::npos && s.find("dots") != std::string::npos && s.find("does not match") == std::string::npos, "summary '%s'", s.c_str());
    }
    return testSummary("vor_marker");
}
