// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LBJ receiver on the generated test signal (clean, then 15 dB and a carrier offset): every message of a cycle must arrive with its
// exact train number, speed, kilometre post and direction. Noise alone must give nothing. The generator makes the signal from the
// decoded frames of the real recordings (a basic message), so this is a round trip through the whole chain.
#include "dect2/lbj/lbj_gen.h"
#include "dect2/lbj/lbj_rx.h"
#include "dect2/lbj/lbj_tel.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static LbjTelemetry run(const SynthConfig& cfg, double rate, double seconds, bool noiseOnly = false, double offsetHz = -lbjTuning().tuneOffsetHz) {
    LbjReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(offsetHz);
    auto syn = makeLbjSynth(cfg, rate);
    std::vector<cf32> buf(32768);
    LbjTelemetry t;
    uint64_t seq = 0;
    unsigned s = 777;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        if (noiseOnly) {
            for (auto& v : buf) { s = s * 1664525u + 1013904223u; const float a = ((s >> 8) & 0xFFFF) / 65536.f - 0.5f; s = s * 1664525u + 1013904223u; v = cf32(a * 0.3f, (((s >> 8) & 0xFFFF) / 65536.f - 0.5f) * 0.3f); }
        } else syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        LbjTelemetry n;
        if (rx.telemetry(n, seq)) { seq = n.seq; t = n; }
    }
    return t;
}

static int countFound(const LbjTelemetry& t, const char* what) {
    int found = 0;
    for (const auto& w : lbj::testMessages()) {
        bool ok = false;
        for (const LbjMessage& m : t.messages)
            if (m.train == w.train && m.speedKmh == w.speed && m.kmMarker == w.km && m.direction == w.direction) { ok = true; break; }
        CHECK(ok, "%s: missing train %s speed %s km %s %s", what, w.train.c_str(), w.speed.c_str(), w.km.c_str(), w.direction.c_str());
        found += ok;
    }
    int stray = 0;
    for (const LbjMessage& m : t.messages) {
        bool known = false;
        for (const auto& w : lbj::testMessages())
            if (m.train == w.train && m.speedKmh == w.speed && m.kmMarker == w.km && m.direction == w.direction) known = true;
        if (!known) { stray++; printf("  stray: %s speed %s km %s %s\n", m.train.c_str(), m.speedKmh.c_str(), m.kmMarker.c_str(), m.direction.c_str()); }
    }
    CHECK(stray == 0, "%s: %d messages that were not sent", what, stray);
    return found;
}

int main() {
    const double rate = 960000;
    const double cycle = 4 * (576 + 32 + 512) / 1200.0 + 4 * 0.6 + 0.5;      // seconds
    {   // the radio's analogue filter (total width basebandHz, centred on its own 0 Hz) must pass the carrier, which sits
        // tuneOffsetHz away from the centre with about +-4.5 kHz of deviation: a 35 kHz filter cut every LBJ signal
        const ModeTuning t = lbjTuning();
        CHECK(t.basebandHz / 2 > t.tuneOffsetHz + 4500 + 6000, "analogue filter %.0f Hz does not pass the carrier at %.0f Hz", t.basebandHz, t.tuneOffsetHz);
    }
    {
        SynthConfig cfg; cfg.snrDb = 40;
        countFound(run(cfg, rate, cycle + 1), "clean");
    }
    {
        SynthConfig cfg; cfg.snrDb = 15; cfg.cfoHz = 1800;
        countFound(run(cfg, rate, cycle + 1), "15 dB, +1.8 kHz");
    }
    {
        SynthConfig cfg; cfg.snrDb = 15; cfg.cfoHz = -2200; cfg.modeOpt[4] = 3700;
        countFound(run(cfg, rate, cycle + 1), "15 dB, -2.2 kHz, deviation 3.7 kHz");
    }
    {   // reversed polarity (an SDR with swapped I and Q) decodes too
        SynthConfig cfg; cfg.snrDb = 30; cfg.modeOpt[1] = 1;
        countFound(run(cfg, rate, cycle + 1), "reversed polarity");
    }
    {
        SynthConfig cfg;
        const LbjTelemetry t = run(cfg, rate, 20, true);
        CHECK(t.messages.empty(), "noise gave %zu messages", t.messages.size());
    }
    {   // 1.92 Msps input
        SynthConfig cfg; cfg.snrDb = 20;
        countFound(run(cfg, 1920000, cycle + 1), "1.92 Msps");
    }
    return testSummary("test_lbj_rx");
}
