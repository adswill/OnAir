// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ROS (HF) detector on the generator's signals (the publicly reported physical layer: one of 144 tones on a 15.625 Hz grid per symbol): 4, 8 and 16 baud,
// then tuning errors of +-150 Hz, a drift, a symbol clock off by 300 ppm, lower sideband, noise at a few SNRs; and the signals that are not ROS (noise, a 2-tone
// FSK, a single carrier) must not be reported. Text is never decoded.
#include "dect2/roshf/roshf_gen.h"
#include "dect2/roshf/roshf_rx.h"
#include "dect2/roshf/roshf_tel.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

static const double kRate = 250000;

static RoshfTelemetry run(RoshfGenConfig c, double sec, int demod = HfDemodUsb) {
    c.rate = kRate;
    c.dialOffsetHz = -20000;
    RoshfGenerator g(c);
    RoshfReceiver r;
    r.configure(kRate);
    r.setSignalOffset(-20000);
    r.setDemod(demod);
    std::vector<cf32> buf(25000);
    const size_t tot = (size_t)(sec * kRate);
    for (size_t i = 0; i < tot; i += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    r.flush();
    RoshfTelemetry t;
    r.telemetry(t, 0);
    return t;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    for (double baud : {16.0, 8.0, 4.0}) {
        RoshfGenConfig c; c.baud = baud; c.snrDb = 99;
        const RoshfTelemetry t = run(c, baud >= 8 ? 24 : 40);
        printf("  clean %g baud: state %d %s, tones %.0f - %.0f Hz, score %.2f, %zu events\n", baud, t.state, t.name.c_str(), t.loHz, t.hiHz, t.score, t.events.size());
        char id[16]; snprintf(id, sizeof id, "ros%d", (int)baud);
        CHECK(t.state == 2 && t.subId == id, "%g baud: state %d sub-mode '%s'", baud, t.state, t.subId.c_str());
        CHECK(t.loHz > 400 && t.hiHz < 2900 && t.spanHz > 1800 && t.spanHz < 2500, "tone range %.0f - %.0f", t.loHz, t.hiHz);
        CHECK(t.markHiHz > t.markLoHz && !t.events.empty(), "marker and event");
        CHECK(t.text.empty() && t.textStatus.find("partial, needs a public spec") != std::string::npos, "text status");
        CHECK(!t.water.empty() && t.water[0].size() == kRoshfWaterBins, "waterfall");
    }
    struct Case { const char* name; double cfo, drift, ppm, snr; bool lsb; };
    const Case cs[] = {
        {"tuning +150 Hz", 150, 0, 0, 99, false}, {"tuning -150 Hz", -150, 0, 0, 99, false}, {"drift 1 Hz/s", 0, 1, 0, 99, false}, {"symbol clock +300 ppm", 0, 0, 300, 99, false},
        {"SNR 10 dB in 2.5 kHz", 0, 0, 0, 10, false}, {"SNR 3 dB in 2.5 kHz", 0, 0, 0, 3, false}, {"lower sideband", 0, 0, 0, 99, true},
        {"all: +100 Hz, 0.5 Hz/s, 200 ppm, 6 dB", 100, 0.5, 200, 6, false},
    };
    // quick run: -150 Hz, 10 dB and the lone drift and clock cases are dropped (the "all" case carries tuning, drift, clock and noise at once);
    // ONAIR_TEST_LONG=1 runs every case
    for (const Case& k : cs) {
        if (!testLong() && (k.cfo == -150 || k.snr == 10 || (k.cfo == 0 && (k.drift != 0 || k.ppm != 0)))) continue;
        RoshfGenConfig c; c.baud = 16; c.cfoHz = k.cfo; c.driftHzPerSec = k.drift; c.clockPpm = k.ppm; c.snrDb = k.snr; c.lsb = k.lsb; c.seed = 3;
        const RoshfTelemetry t = run(c, 26, k.lsb ? HfDemodLsb : HfDemodUsb);
        printf("  %-40s state %d %s tones %.0f - %.0f\n", k.name, t.state, t.subId.c_str(), t.loHz, t.hiHz);
        CHECK(t.state == 2 && t.subId == "ros16", "%s: state %d sub-mode '%s'", k.name, t.state, t.subId.c_str());
    }
    {   // not ROS
        const char* names[] = {"noise", "2-tone FSK", "single carrier"};
        for (int kind = 1; kind <= 3; kind++) {
            RoshfGenConfig c; c.kind = kind; c.snrDb = 20; c.seed = 9;
            const RoshfTelemetry t = run(c, 26);
            printf("  %-40s state %d %s\n", names[kind - 1], t.state, t.subId.c_str());
            CHECK(t.state != 2 && t.events.empty(), "%s reported as ROS: %s", names[kind - 1], t.name.c_str());
            if (kind == 1 || kind == 3) CHECK(t.state == 0, "%s: state %d", names[kind - 1], t.state);
        }
    }
    {   // the log survives a retune and Clear empties it
        RoshfGenConfig c; c.baud = 16; c.snrDb = 99; c.rate = kRate; c.dialOffsetHz = -20000;
        RoshfGenerator g(c);
        RoshfReceiver r;
        r.configure(kRate); r.setSignalOffset(-20000);
        std::vector<cf32> buf(25000);
        for (size_t i = 0; i < (size_t)(24 * kRate); i += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
        r.flush();
        RoshfTelemetry t; r.telemetry(t, 0);
        CHECK(!t.events.empty(), "event");
        r.clearResults();
        RoshfTelemetry t2; r.telemetry(t2, t.seq);
        CHECK(t2.events.empty() && t2.blocksOk == 0, "cleared");
    }
    return testSummary("roshf rx");
}
