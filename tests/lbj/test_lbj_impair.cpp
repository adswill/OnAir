// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LBJ receiver under real-world impairments, every combination that matters for a locomotive transmitter: a carrier offset (crystal error of the
// transmitter or of the radio, a wrongly set channel offset), a different frequency deviation, low signal to noise ratio, reversed polarity and
// other input rates. The receiver must decode all messages of the cycle and never invent one.
#include "dect2/lbj/lbj_gen.h"
#include "dect2/lbj/lbj_rx.h"
#include "dect2/lbj/lbj_tel.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Case { double snr, cfo, dev, rate, rxOff; int seed, inv; };

static void runCase(const Case& c) {
    SynthConfig cfg;
    cfg.snrDb = c.snr; cfg.cfoHz = c.cfo; cfg.modeOpt[4] = (int)c.dev; cfg.modeOpt[3] = c.seed; cfg.modeOpt[1] = c.inv;
    LbjReceiver rx;
    rx.configure(c.rate);
    rx.setSignalOffset(-lbjTuning().tuneOffsetHz + c.rxOff);
    auto syn = makeLbjSynth(cfg, c.rate);
    std::vector<cf32> buf(32768);
    const double seconds = (4 * (576 + 32 + 512) / 1200.0 + 4 * 0.6) * 2 + 1.5;      // a bit more than two cycles
    for (double d = 0; d < seconds * c.rate; d += (double)buf.size()) {
        syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
    }
    LbjTelemetry t;
    rx.telemetry(t, 0);
    int found = 0, stray = 0;
    auto sameField = [](const std::string& got, const std::string& want) { return got.empty() || got == want; };   // a lost field stays empty, never wrong
    for (const auto& w : lbj::testMessages()) {
        bool ok = false;
        for (const LbjMessage& m : t.messages)
            if (m.train == w.train && m.speedKmh == w.speed && m.kmMarker == w.km && m.direction == w.direction) ok = true;
        found += ok;
    }
    for (const LbjMessage& m : t.messages) {
        bool known = false;
        for (const auto& w : lbj::testMessages())
            if (m.train == w.train && sameField(m.speedKmh, w.speed) && sameField(m.kmMarker, w.km) && m.direction == w.direction) known = true;
        stray += !known;
    }
    CHECK(found == 4 && stray == 0, "snr %.0f cfo %+.0f dev %.0f rate %.0f rx-offset %+.0f seed %d inv %d: found %d/4, %d stray", c.snr, c.cfo, c.dev, c.rate, c.rxOff, c.seed, c.inv, found, stray);
}

int main() {
    // signal to noise ratio (carrier over noise in 25 kHz)
    for (double snr : {4.0, 8.0, 20.0, 50.0}) runCase({snr, 0, 4200, 960000, 0, 1, 0});
    // carrier offset of the transmitter, both signs, up to +-3.5 kHz, at a modest SNR
    for (double cfo : {-3500.0, -2000.0, -700.0, 700.0, 2000.0, 3500.0}) runCase({10, cfo, 4200, 960000, 0, 2, 0});
    // the receiver's channel offset wrongly set (off-centre) by +-2 kHz
    for (double off : {-2000.0, 2000.0}) runCase({10, 0, 4200, 960000, off, 3, 0});
    // frequency deviation of 3.2 to 5 kHz (transmitters differ), with an offset
    for (double dev : {3200.0, 3700.0, 4800.0, 5000.0}) runCase({10, 1000, dev, 960000, 0, 4, 0});
    // reversed polarity, other input rates
    runCase({10, 500, 4200, 960000, 0, 5, 1});
    runCase({10, -500, 4200, 1920000, 0, 6, 0});
    runCase({10, 500, 4200, 2400000, 0, 7, 1});
    // worst case together: low SNR, offset, deviation at the edge
    runCase({6, 2500, 3500, 960000, 500, 8, 0});
    return testSummary("test_lbj_impair");
}
