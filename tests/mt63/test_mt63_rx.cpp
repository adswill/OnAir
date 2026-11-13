// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MT63 receiver against generated signals: the three bandwidths with short and long interleave, offsets from the dial and a tuning error,
// noise, fading and an echo, a symbol clock error, drift, a signal that starts late and one that stops, two different lengths of text,
// noise only (no text), and a block that is not MT63.
#include "dect2/mt63/mt63_gen.h"
#include "dect2/mt63/mt63_rx.h"
#include "dect2/mt63/mt63_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRate = 250000;
constexpr double kOff = -20000;   // the dial sits 20 kHz below the centre, as with the radio

static Mt63Telemetry run(const std::vector<Mt63GenSignal>& sigs, double secs, double cfo = 0, int forceBw = 0, int forceIntl = 0, double noise = 0.02, uint32_t seed = 3) {
    Mt63GenConfig c;
    c.rate = kRate; c.dialOffsetHz = kOff; c.cfoHz = cfo; c.signals = sigs; c.seed = seed; c.noiseSigma = noise;
    Mt63Generator g(c);
    Mt63Receiver r;
    r.configure(kRate);
    r.setSignalOffset(kOff);
    r.setBandwidth(forceBw);
    r.setInterleave(forceIntl);
    Mt63Telemetry res;
    std::vector<cf32> x(25000);
    uint64_t seq = 0;
    for (double t = 0; t < secs; t += 0.1) {
        g.generate(x.data(), x.size());
        r.feed(x.data(), x.size());
        Mt63Telemetry tt;
        if (r.telemetry(tt, seq)) { seq = tt.seq; res = std::move(tt); }
    }
    return res;
}

static Mt63GenSignal sig(int bw, bool lng, double centre, double snr, const std::string& text = "CQ CQ DE LY2ABC LY2ABC K\n") {
    Mt63GenSignal s;
    s.bandwidth = bw; s.longInterleave = lng; s.centreHz = centre; s.snrDb = snr; s.text = text; s.pauseSec = 1.0;
    return s;
}

static double secsFor(int bw, bool lng) { return 3.0 + (lng ? 64 : 32) * 0.1 * (1000.0 / bw) * 2.2 + 10 * 0.1 * (1000.0 / bw) + 12; }

static void expect(const char* name, const Mt63Telemetry& t, int bw, int intl, const std::string& want = "LY2ABC") {
    const bool ok = t.text.find(want) != std::string::npos;
    printf("  %-26s bw %4d intl %s centre %7.1f snr %4.1f q %4.1f chars %llu %s\n", name, t.bandwidth, mt63InterleaveName(t.interleave), t.centreHz, t.snrDb, t.quality, (unsigned long long)t.chars, ok ? "ok" : "MISS");
    CHECK(ok, "%s: \"%s\" not in \"%s\" (state %d)", name, want.c_str(), t.text.c_str(), t.state);
    CHECK(t.bandwidth == bw, "%s: bandwidth %d, wanted %d", name, t.bandwidth, bw);
    if (intl) CHECK(t.interleave == intl, "%s: interleave %d, wanted %d", name, t.interleave, intl);
}

int main() {
    for (int bw : {500, 1000, 2000})
        for (bool lng : {false, true}) {
            char nm[40]; snprintf(nm, sizeof nm, "clean %d %s", bw, lng ? "long" : "short");
            const double centre = bw == 2000 ? 2000 : 1500;
            expect(nm, run({sig(bw, lng, centre, 20)}, secsFor(bw, lng)), bw, lng ? 2 : 1);
        }
    {   // tuning: the dial off by +-80 Hz, the block off-centre
        auto t = run({sig(1000, false, 1500, 15)}, 40, 80);
        expect("cfo +80 Hz", t, 1000, 1);
        t = run({sig(1000, true, 1700, 15)}, 50, -90);
        expect("cfo -90 Hz long", t, 1000, 2);
        t = run({sig(500, false, 700, 15)}, 60, 45);
        expect("500 low in the band", t, 500, 1);
        t = run({sig(1000, false, 3100, 15)}, 40);
        expect("1000 near the top", t, 1000, 1);
        t = run({sig(2000, false, 2800, 15)}, 40);
        expect("2000 near the top", t, 2000, 1);
    }
    {   // noise
        auto t = run({sig(1000, false, 1500, 4)}, 70);
        expect("1000 short at 4 dB", t, 1000, 1);
        t = run({sig(1000, true, 1500, 2)}, 90);
        expect("1000 long at 2 dB", t, 1000, 2);
    }
    {   // fading and echo
        Mt63GenSignal s = sig(1000, true, 1500, 14);
        s.fadeDepth = 0.8; s.fadeHz = 0.25;
        expect("fading 0.8 at 0.25 Hz", run({s}, 90), 1000, 2);
        s = sig(1000, true, 1500, 14);
        s.echoMs = 4; s.echoDb = -6;
        expect("echo 4 ms", run({s}, 90), 1000, 2);
    }
    {   // clock error and drift
        Mt63GenSignal s = sig(1000, false, 1500, 14);
        s.clockPpm = 150; s.driftHzPerMin = 6;
        expect("150 ppm and 6 Hz/min", run({s}, 60), 1000, 1);
    }
    {   // the forced settings find it as well
        expect("forced 1000 / short", run({sig(1000, false, 1500, 14)}, 40, 0, 1000, 1), 1000, 1);
        // forced to the wrong interleave: no text
        const auto t = run({sig(1000, false, 1500, 14)}, 40, 0, 0, 2);
        CHECK(t.text.find("LY2ABC") == std::string::npos, "forced wrong interleave still decoded \"%s\"", t.text.c_str());
    }
    {   // a different text, long
        const std::string msg = "The quick brown fox jumps over the lazy dog 0123456789\nSP5ABC DE OK1XYZ\n";
        expect("long text", run({sig(1000, false, 1500, 16, msg)}, 60), 1000, 1, "quick brown fox jumps over the lazy dog 0123456789");
    }
    {   // nothing but noise: no text, no lock
        const auto t = run({}, 40);
        CHECK(t.text.empty() && t.state == 0, "noise: text \"%s\" state %d", t.text.c_str(), t.state);
        printf("  noise only: state %d, %llu chars\n", t.state, (unsigned long long)t.chars);
    }
    {   // a carrier-free flat block that is not MT63 (white noise 1 kHz wide): no text
        Mt63GenSignal s = sig(1000, false, 1500, 14);
        s.text = ""; s.startSec = 1e6;   // idle only: the comb is there but no characters; must still lock without text
        const auto t = run({s}, 40);
        printf("  idle only: state %d bw %d chars %llu text \"%s\"\n", t.state, t.bandwidth, (unsigned long long)t.chars, t.text.c_str());
        CHECK(t.bandwidth == 1000 && t.state == 2, "idle MT63 not locked (state %d bw %d)", t.state, t.bandwidth);
        CHECK(t.text.empty(), "idle text \"%s\"", t.text.c_str());
    }
    return testSummary("mt63 rx");
}
