// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C-QUAM under real-world impairments: tuning error and a station off the dial, sample rates, noise (the weakest stereo is printed, and
// weak signals never give a false stereo), flat fading, a retune in the middle and no pilot on a noisy mono station.
#include "dect2/cquam/cquam_gen.h"
#include "dect2/cquam/cquam_rx.h"
#include "dect2/cquam/cquam_tel.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static CquamTelemetry run(const SynthConfig& sc, double secs, double rate = 1000000, bool retune = false) {
    auto g = makeCquamSynth(sc, rate);
    CquamReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-cquamTuning().tuneOffsetHz);
    rx.setSilent(true);
    CquamTelemetry res;
    std::vector<cf32> x((size_t)(rate / 10));
    uint64_t seq = 0;
    for (double t = 0; t < secs; t += 0.1) {
        g->generate(x.data(), x.size());
        if (retune && t > secs * 0.5 && t < secs * 0.5 + 0.05) rx.reset();
        rx.feed(x.data(), x.size());
        CquamTelemetry tt;
        if (rx.telemetry(tt, seq)) { seq = tt.seq; res = tt; }
    }
    return res;
}
static SynthConfig cfg(int kind, double snr = 35) { SynthConfig s; s.snrDb = snr; s.modeOpt[0] = kind; return s; }

int main() {
    for (double cfo : {-400.0, -150.0, -20.0, 0.0, 35.0, 180.0, 400.0}) {   // the radio's tuning error
        SynthConfig s = cfg(0); s.cfoHz = cfo;
        const auto t = run(s, 10);
        printf("  cfo %+5.0f Hz: %s\n", cfo, cquamSummary(t).c_str());
        CHECK(t.state == 2 && std::fabs(t.cfoHz - cfo) < 3, "cfo %.0f: state %d, loop %.1f Hz", cfo, t.state, t.cfoHz);
    }
    for (double off : {-250.0, 120.0}) {   // the station off the dial: a channel offset brings it in (the user clicked on it)
        SynthConfig s = cfg(0); s.modeVal[0] = off;
        const auto t = run(s, 10);
        CHECK(t.state == 2 && std::fabs(t.cfoHz - off) < 3, "station %.0f Hz off: state %d loop %.1f", off, t.state, t.cfoHz);
    }
    for (double rate : {250000.0, 500000.0, 2000000.0}) {
        const auto t = run(cfg(0), 10, rate);
        printf("  rate %.0f: %s\n", rate, cquamSummary(t).c_str());
        CHECK(t.state == 2, "rate %.0f: state %d", rate, t.state);
    }
    double weakest = 99;
    for (double snr : {35.0, 30.0, 25.0, 20.0, 16.0, 12.0}) {   // carrier over the noise in 10 kHz
        const auto t = run(cfg(0, snr), 14);
        printf("  C/N %4.0f dB: state %d, pilot %.1f dB (%.1f %%)\n", snr, t.state, t.pilotDb, t.pilotPct);
        if (t.state == 2) weakest = snr;
        if (snr >= 20) CHECK(t.state == 2, "stereo lost at %.0f dB", snr);
        CHECK(t.state >= 1 || snr < 14, "no carrier at %.0f dB", snr);
    }
    printf("  weakest stereo: %.0f dB\n", weakest);
    for (double snr : {25.0, 12.0, 8.0}) {   // never a stereo flag on a mono station, whatever the noise
        for (uint32_t k = 0; k < 2; k++) {
            SynthConfig s = cfg(1, snr); s.modeVal[0] = k * 40.0;
            const auto t = run(s, 14);
            CHECK(!t.stereo && t.state <= 1, "false stereo at %.0f dB (%u)", snr, k);
        }
    }
    for (double d : {0.5, 0.8}) {   // flat fading: the carrier swings between 1 and 1 - d at 0.5 Hz
        SynthConfig s = cfg(0, 30); s.modeVal[1] = d; s.modeVal[2] = 0.5;
        const auto t = run(s, 16);
        printf("  fading %.1f: %s\n", d, cquamSummary(t).c_str());
        CHECK(t.state >= 1 && t.stereo, "fading depth %.1f: state %d stereo %d", d, t.state, t.stereo);
    }
    {   // a retune in the middle: the receiver starts again and finds the station
        const auto t = run(cfg(0), 14, 1000000, true);
        CHECK(t.state == 2, "after reset: state %d", t.state);
    }
    return testSummary("cquam sweep");
}
