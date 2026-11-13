// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// QO-100 tools on the test signal, fed straight to the receiver: the CW beacons are found and the LNB error (+27 kHz, drifting 120 Hz/min)
// measured within 50 Hz, the drift is followed, a test carrier's corrected frequency is right within 20 Hz, a 200 kHz error and an
// inverted (10600 MHz) LNB lock too, the whole downlink at 10 Msps shows the DATV beacon at 1.5 Msym/s, SSB listening gives audio, and
// noise alone never locks.
#include "dect2/qo100/qo100_dsp.h"
#include "dect2/qo100/qo100_gen.h"
#include "dect2/qo100/qo100_rx.h"
#include "dect2/qo100/qo100_tel.h"
#include "dect2/source/source.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Run {
    Qo100Telemetry last;
    double lockAt = -1;
    bool everLocked = false, everCorr = false;
    uint64_t audio = 0;
    double audioPow = 0;
};

static Run run(const SynthConfig& sc, double fs, double centreRf, double lo, double secs, bool listen = false, double dial = 0, bool autoCorr = true) {
    Run r;
    Qo100Receiver rx;
    rx.configure(fs);
    rx.setAutoCorrection(autoCorr);
    rx.setLo(lo);
    rx.setCenterHz(qo100::ifOf(centreRf, lo));
    if (listen) {
        rx.setSilent(true);
        rx.setDialHz(dial);
        rx.setListen(true);
        rx.setListenTap([&r](const float* x, size_t n) { for (size_t i = 0; i < n; i++) r.audioPow += (double)x[i] * x[i]; r.audio += n; });
    }
    auto syn = makeQo100Synth(sc, fs);
    std::vector<cf32> buf(20000);
    uint64_t seq = 0, done = 0;
    while (done < (uint64_t)(secs * fs)) {
        syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        done += buf.size();
        Qo100Telemetry t;
        if (rx.telemetry(t, seq)) {
            seq = t.seq;
            if (t.state == 2 && r.lockAt < 0) r.lockAt = (double)done / fs;
            if (t.state == 2) r.everLocked = true;
            if (t.corrValid) r.everCorr = true;
            r.last = std::move(t);
        }
    }
    return r;
}

// the measured error against the test signal's (each measurement covers the 0.8 s before it)
static double worstErr(const Qo100Telemetry& t, const SynthConfig& sc, double after) {
    double w = 0;
    for (size_t i = 0; i < t.histT.size(); i++) {
        if (t.histT[i] < after) continue;
        w = std::max(w, std::fabs(t.histErr[i] - qo100SynthErrHz(sc, t.histT[i] - 0.45)));
    }
    return w;
}

static const Qo100Signal* nearest(const Qo100Telemetry& t, double hz) {
    const Qo100Signal* b = nullptr;
    for (const auto& s : t.signals) if (!b || std::fabs(s.rfHz - hz) < std::fabs(b->rfHz - hz)) b = &s;
    return b;
}

int main() {
    // the maths
    {
        std::vector<double> t = {0, 10, 20, 30}, v = {5, 25, 45, 65};
        double s = 0;
        CHECK(qo100::slopePerMin(t, v, s) && std::fabs(s - 120) < 1e-9, "slope %.3f", s);
        CHECK(std::fabs(qo100::ifOf(10489.5e6, 9750e6) - 739.5e6) < 1 && std::fabs(qo100::ifOf(10489.5e6, 10600e6) - 110.5e6) < 1, "IF");
        CHECK(std::fabs(qo100::rfOf(739.5e6, 9750e6) - 10489.5e6) < 1 && std::fabs(qo100::rfOf(110.5e6, 10600e6) - 10489.5e6) < 1, "RF");
        CHECK(qo100Tuning().stdMode == 55 && std::fabs(qo100Tuning().defMhz - 744.0) < 1e-9, "tuning");
    }
    const double lo = 9750e6;
    // 0: automatic frequency correction off (a GPS-locked LNB): the error is still measured, but the frequencies are not moved by it
    {
        SynthConfig sc;
        Run on = run(sc, 2e6, qo100::kNarrowCentreHz, lo, 12), off = run(sc, 2e6, qo100::kNarrowCentreHz, lo, 12, false, 0, false);
        CHECK(on.last.corrApplied && !off.last.corrApplied, "corrApplied %d %d", on.last.corrApplied, off.last.corrApplied);
        CHECK(off.last.corrValid && std::fabs(off.last.lnbErrHz - on.last.lnbErrHz) < 100, "error still measured: %.1f vs %.1f Hz", off.last.lnbErrHz, on.last.lnbErrHz);
        const Qo100Signal* a = nearest(on.last, kQo100SsbDialsHz[0]);
        const Qo100Signal* b = nearest(off.last, kQo100SsbDialsHz[0]);
        CHECK(a && b && std::fabs((a->rfHz - b->rfHz) - on.last.lnbErrHz) < 3000, "station moved by %.1f Hz, the error is %.1f Hz", a && b ? a->rfHz - b->rfHz : 0.0, on.last.lnbErrHz);
    }
    // 1: the narrowband transponder at 2 Msps, +27 kHz drifting 120 Hz/min, listening on the first SSB station
    {
        SynthConfig sc;
        const double fs = 2e6, secs = 30;
        Run r = run(sc, fs, qo100::kNarrowCentreHz, lo, secs, true, kQo100SsbDialsHz[0]);
        const Qo100Telemetry& t = r.last;
        printf("  narrowband: %s, locked at %.1f s\n", qo100Summary(t).c_str(), r.lockAt);
        CHECK(r.lockAt > 0 && r.lockAt < 5, "locked at %.1f s", r.lockAt);
        CHECK(t.state == 2 && t.corrValid, "state %d", t.state);
        const double truth = qo100SynthErrHz(sc, secs - 0.5);
        CHECK(std::fabs(t.lnbErrHz - truth) < 50, "LNB error %.1f Hz, the signal's %.1f Hz", t.lnbErrHz, truth);
        const double w = worstErr(t, sc, r.lockAt + 1);
        CHECK(w < 50, "worst error over the run %.1f Hz", w);
        CHECK(std::fabs(t.lnbErrPpm - truth / lo * 1e6) < 0.01, "ppm %.4f", t.lnbErrPpm);
        // the drift: the history follows it, the rate is right
        CHECK(t.histT.size() > 40, "%zu measurements", t.histT.size());
        if (t.histT.size() > 10) {
            const double d = t.histErr.back() - t.histErr.front(), want = qo100SynthErrHz(sc, t.histT.back()) - qo100SynthErrHz(sc, t.histT.front());
            CHECK(std::fabs(d - want) < 20, "drift over the history %.1f Hz, the signal's %.1f Hz", d, want);
        }
        CHECK(t.driftValid && std::fabs(t.driftHzPerMin - kQo100SynthDriftHzPerMin) < 25, "drift %.1f Hz/min", t.driftHzPerMin);
        CHECK(t.beacons[0].found && t.beacons[2].found && t.beacons[1].found, "beacons %d %d %d", t.beacons[0].found, t.beacons[1].found, t.beacons[2].found);
        // the test carrier, corrected
        const Qo100Signal* c = nearest(t, kQo100TestCarrierHz);
        CHECK(c && std::fabs(c->rfHz - kQo100TestCarrierHz) < 20, "test carrier at %.1f Hz from where it is", c ? c->rfHz - kQo100TestCarrierHz : 1e9);
        // the SSB stations show up as signals near their dials
        int ssb = 0;
        for (double d : kQo100SsbDialsHz) { const Qo100Signal* s = nearest(t, d + 1100); if (s && std::fabs(s->rfHz - d - 1200) < 1500) ssb++; }
        CHECK(ssb == 4, "%d of 4 SSB stations", ssb);
        // the narrowband spectrum covers the transponder in corrected frequencies
        CHECK(!t.nb.empty() && t.nbF0Hz <= qo100::kNbLoHz && t.nbF0Hz + t.nb.size() * t.nbBinHz >= qo100::kNbHiHz && t.nbBinHz < 80, "nb %.3f MHz + %zu x %.1f Hz", t.nbF0Hz / 1e6, t.nb.size(), t.nbBinHz);
        CHECK(r.audio > (uint64_t)(8000 * (secs - 2)) && r.audioPow / std::max<uint64_t>(1, r.audio) > 1e-3, "audio %llu samples, power %.2g", (unsigned long long)r.audio, r.audioPow / std::max<uint64_t>(1, r.audio));
        CHECK(!t.wbOk, "the wideband transponder is not in a 2 Msps band at 10490.1 MHz");
    }
    // 2: a cheap LNB 200 kHz off
    {
        SynthConfig sc;
        sc.modeVal[0] = 200e3 - kQo100SynthErrHz;
        Run r = run(sc, 2e6, qo100::kNarrowCentreHz, lo, 8);
        const double truth = qo100SynthErrHz(sc, 7.5);
        printf("  200 kHz: %s\n", qo100Summary(r.last).c_str());
        CHECK(r.last.state == 2 && std::fabs(r.last.lnbErrHz - truth) < 50, "state %d, LNB error %.1f Hz, the signal's %.1f Hz", r.last.state, r.last.lnbErrHz, truth);
    }
    // 3: an LNB with its oscillator above the downlink (10600 MHz): the spectrum arrives mirrored
    {
        SynthConfig sc;
        sc.modeVal[3] = 10600;
        sc.modeVal[0] = -40e3 - kQo100SynthErrHz;
        Run r = run(sc, 2e6, qo100::kNarrowCentreHz, 10600e6, 8);
        const double truth = qo100SynthErrHz(sc, 7.5);
        printf("  inverted: %s\n", qo100Summary(r.last).c_str());
        CHECK(r.last.inverted && r.last.state == 2 && std::fabs(r.last.lnbErrHz - truth) < 50, "state %d, LNB error %.1f Hz, the signal's %.1f Hz", r.last.state, r.last.lnbErrHz, truth);
        const Qo100Signal* c = nearest(r.last, kQo100TestCarrierHz);
        CHECK(c && std::fabs(c->rfHz - kQo100TestCarrierHz) < 20, "inverted: test carrier %.1f Hz off", c ? c->rfHz - kQo100TestCarrierHz : 1e9);
    }
    // 4: the whole downlink at 10 Msps: the beacons lock and the DATV carriers are measured
    {
        SynthConfig sc;
        Run r = run(sc, 10e6, qo100::kWholeCentreHz, lo, 5);
        const Qo100Telemetry& t = r.last;
        printf("  whole downlink: %s\n", qo100Summary(t).c_str());
        CHECK(t.state == 2 && std::fabs(t.lnbErrHz - qo100SynthErrHz(sc, 4.5)) < 50, "state %d, LNB error %.1f Hz", t.state, t.lnbErrHz);
        CHECK(t.wbOk, "wideband in the band");
        const Qo100Datv* b = nullptr;
        const Qo100Datv* s2 = nullptr;
        for (const auto& d : t.datv) {
            printf("    DATV %.4f MHz, %.0f ks/s, %.1f dB%s%s\n", d.rfHz / 1e6, d.symRate / 1e3, d.snrDb, d.looksDvbs ? ", DVB-S" : "", d.beacon ? ", beacon" : "");
            if (std::fabs(d.rfHz - qo100::kDatvBeaconHz) < 100e3) b = &d;
            if (std::fabs(d.rfHz - kQo100Datv2Hz) < 50e3) s2 = &d;
        }
        CHECK(b && b->beacon && b->looksDvbs && std::fabs(b->symRate - 1.5e6) < 0.15e6, "DATV beacon %s", b ? "measured wrong" : "not found");
        CHECK(b && std::fabs(b->ifHz - qo100::ifOf(qo100::kDatvBeaconHz - t.lnbErrHz, lo)) < 50e3, "beacon IF %.3f MHz", b ? b->ifHz / 1e6 : 0);
        CHECK(s2 && std::fabs(s2->symRate - kQo100Datv2Sym) < 40e3, "333 ks/s carrier %s", s2 ? "measured wrong" : "not found");
    }
    // 5: noise only: no lock, no correction
    {
        SynthConfig sc;
        sc.modeOpt[0] = 1;
        Run r = run(sc, 2e6, qo100::kNarrowCentreHz, lo, 12);
        printf("  noise: %s\n", qo100Summary(r.last).c_str());
        CHECK(!r.everLocked && !r.everCorr && r.last.state == 1, "noise locked (state %d)", r.last.state);
    }
    printf(fails ? "qo100 rx: %d FAILED\n" : "qo100 rx: all passed\n", fails);
    return fails ? 1 : 0;
}
