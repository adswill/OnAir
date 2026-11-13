// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// IFKP receiver against generated signals: the three speeds, noise, tuning error, off-centre, symbol clock error, drift, fading, echo,
// a signal that starts late, a forced speed, noise only and an FSQ signal (other tone spacing: not IFKP).
#include "dect2/fsq/kbd33_gen.h"
#include "dect2/fsq/kbd33_station.h"
#include "dect2/fsq/kbd33.h"
#include "dect2/ifkp/ifkp_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRate = 250000;
constexpr double kOff = -20000;   // the dial sits 20 kHz below the centre, as with the radio

static IfkpTelemetry run(const std::vector<Kbd33GenSignal>& sigs, double secs, double cfo = 0, int forceSpeed = -1, double noise = 0.02, uint32_t seed = 3) {
    Kbd33GenConfig c;
    c.rate = kRate; c.dialOffsetHz = kOff; c.cfoHz = cfo; c.signals = sigs; c.seed = seed; c.noiseSigma = noise;
    Kbd33Generator g(c);
    IfkpReceiver r;
    r.configure(kRate);
    r.setSignalOffset(kOff);
    r.setSpeed(forceSpeed);
    IfkpTelemetry res;
    std::vector<cf32> x(25000);
    uint64_t seq = 0;
    for (double t = 0; t < secs; t += 0.1) {
        g.generate(x.data(), x.size());
        r.feed(x.data(), x.size());
        IfkpTelemetry tt;
        if (r.telemetry(tt, seq)) { seq = tt.seq; res = std::move(tt); }
    }
    return res;
}

static const char* kText = "cq cq de LY2ABC LY2ABC ifkp test pse k\n";

static Kbd33GenSignal sig(int speed, double centre, double snr) {
    Kbd33GenSignal s;
    s.ifkp = true; s.speed = speed; s.centreHz = centre; s.snrDb = snr;
    s.text = kText;
    s.pauseSec = 1.5; s.startSec = 1.0;
    return s;
}

// time to send the text twice, plus the time to find the signal
static double secsFor(int speed) { return 12 + 2.0 * 50 * kbd33::ifkpSpec(speed).symSec + 3 * kbd33::ifkpSpec(speed).symSec * 24 * 0.3; }

static void expect(const char* name, const IfkpTelemetry& t, int speed) {
    const bool ok = t.text.find("cq cq de LY2ABC LY2ABC ifkp test pse k") != std::string::npos;
    printf("  %-24s speed %s centre %6.1f..%6.1f snr %5.1f q %.2f chars %llu drop %llu %s\n", name, t.speed >= 0 ? kbd33::speedName(true, t.speed) : "-", t.occLoHz, t.occHiHz, t.snrDb, t.quality, (unsigned long long)t.chars, (unsigned long long)t.blocksBad, ok ? "ok" : "MISS");
    CHECK(ok, "%s: text \"%s\" (state %d)", name, t.text.c_str(), t.state);
    if (speed >= 0) CHECK(t.speed == speed, "%s: speed %d, wanted %d", name, t.speed, speed);
}

int main() {
    const ModeTuning mt = ifkpTuning();
    CHECK(mt.stdMode == 118, "tuning");
    for (int sp = 0; sp < kbd33::kIfkpSpeeds; sp++) {
        char nm[40]; snprintf(nm, sizeof nm, "clean speed %s", kbd33::speedName(true, sp));
        expect(nm, run({sig(sp, 1500, 15)}, secsFor(sp)), sp);
    }
    for (int sp : {0, 1, 2}) {
        char nm[40]; snprintf(nm, sizeof nm, "noisy speed %s", kbd33::speedName(true, sp));
        expect(nm, run({sig(sp, 1500, 4)}, secsFor(sp) + 12), sp);
    }
    expect("cfo +90 Hz", run({sig(1, 1500, 12)}, secsFor(1), 90), 1);
    expect("cfo -90 Hz", run({sig(1, 1500, 12)}, secsFor(1), -90), 1);
    expect("centre 600 Hz", run({sig(1, 600, 12)}, secsFor(1)), 1);
    expect("centre 3300 Hz", run({sig(1, 3300, 12)}, secsFor(1)), 1);
    expect("centre 3300 2.0", run({sig(2, 3300, 12)}, secsFor(2)), 2);
    for (int sp = 0; sp < kbd33::kIfkpSpeeds; sp++) {   // every speed with a tuning error, a clock error and an off-centre band together
        for (int sgn : {1, -1}) {
            auto s = sig(sp, sgn > 0 ? 2800 : 700, 10);
            s.clockPpm = 200 * sgn;
            s.driftHzPerMin = 10 * sgn;
            char nm[40]; snprintf(nm, sizeof nm, "all together %s %c", kbd33::speedName(true, sp), sgn > 0 ? '+' : '-');
            expect(nm, run({s}, secsFor(sp) + 6, 70 * sgn), sp);
        }
    }
    {
        auto s = sig(1, 1500, 12); s.clockPpm = 250;
        expect("clock +250 ppm", run({s}, secsFor(1)), 1);
        s.clockPpm = -250;
        expect("clock -250 ppm", run({s}, secsFor(1)), 1);
        auto f = sig(2, 1500, 12); f.clockPpm = 300;
        expect("clock +300 ppm 2.0", run({f}, secsFor(2)), 2);
    }
    { auto s = sig(1, 1500, 12); s.driftHzPerMin = 20; expect("drift 20 Hz/min", run({s}, secsFor(1)), 1); }
    { auto s = sig(1, 1500, 12); s.driftHzPerMin = -30; expect("drift -30 Hz/min", run({s}, secsFor(1)), 1); }
    { auto s = sig(1, 1500, 14); s.fadeDepth = 0.6; s.fadeHz = 0.15; expect("fading", run({s}, secsFor(1) + 20), 1); }
    { auto s = sig(1, 1500, 14); s.echoMs = 4; s.echoDb = -6; expect("echo 4 ms", run({s}, secsFor(1)), 1); }
    { auto s = sig(1, 1500, 12); s.startSec = 20; expect("starts at 20 s", run({s}, secsFor(1) + 20), 1); }
    expect("forced right speed", run({sig(1, 1500, 15)}, secsFor(1), 0, 1), 1);
    {
        const IfkpTelemetry t = run({sig(1, 1500, 15)}, secsFor(1), 0, 2);
        CHECK(t.text.find("ifkp test") == std::string::npos, "a wrong forced speed gave the text");
    }
    {
        const IfkpTelemetry n = run({}, 40);
        CHECK(n.chars == 0 && n.text.empty() && n.state == 0, "noise only: %llu chars, state %d", (unsigned long long)n.chars, n.state);
        Kbd33GenSignal k; k.ifkp = false; k.speed = 2; k.centreHz = 1500; k.snrDb = 12; k.text = fsqFrame("LY2ABC", "LY2XYZ? snr please", true);
        const IfkpTelemetry o = run({k}, 60);
        CHECK(o.text.find("snr please") == std::string::npos, "an FSQ signal read as IFKP: \"%s\"", o.text.c_str());
    }
    {
        auto a = sig(1, 1000, 14);
        auto b = sig(0, 2600, 8);
        expect("two signals", run({a, b}, secsFor(1) + 10), 1);
    }
    {
        const IfkpTelemetry t = run({sig(1, 1500, 15)}, secsFor(1));
        CHECK(t.state == 2 && t.dataValid && t.speed == 1 && std::fabs(t.baud - 3.906) < 0.01, "state %d speed %d baud %.3f", t.state, t.speed, t.baud);
        CHECK(!t.spectrum.empty() && t.toneUse.size() == kbd33::Rx::kPos && t.bandHz > 380 && t.bandHz < 392, "spectrum %zu tones %zu band %.0f", t.spectrum.size(), t.toneUse.size(), t.bandHz);
        CHECK(t.snrDb > 8 && t.snrDb < 22, "SNR %.1f", t.snrDb);
        CHECK(t.msgs.empty(), "IFKP has no messages");
        CHECK(!t.calls.empty() && t.calls[0] == "LY2ABC", "callsign list");
    }
    return testSummary("ifkp rx");
}
