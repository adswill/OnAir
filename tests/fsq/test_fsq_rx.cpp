// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FSQ receiver against generated signals: the five speeds, noise, tuning error, off-centre, symbol clock error, drift, fading, echo,
// a signal that starts late, a forced speed, directed messages (sender, addressee, command), noise only, a steady carrier and an IFKP signal
// (other tone spacing: not FSQ).
#include "dect2/fsq/kbd33_gen.h"
#include "dect2/fsq/kbd33_station.h"
#include "dect2/fsq/kbd33.h"
#include "dect2/fsq/fsq_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

constexpr double kRate = 250000;
constexpr double kOff = -20000;   // the dial sits 20 kHz below the centre, as with the radio

static FsqTelemetry run(const std::vector<Kbd33GenSignal>& sigs, double secs, double cfo = 0, int forceSpeed = -1, double noise = 0.02, uint32_t seed = 3, const char* call = "") {
    Kbd33GenConfig c;
    c.rate = kRate; c.dialOffsetHz = kOff; c.cfoHz = cfo; c.signals = sigs; c.seed = seed; c.noiseSigma = noise;
    Kbd33Generator g(c);
    FsqReceiver r;
    r.configure(kRate);
    r.setSignalOffset(kOff);
    r.setSpeed(forceSpeed);
    r.setMyCall(call);
    FsqTelemetry res;
    std::vector<cf32> x(25000);
    uint64_t seq = 0;
    for (double t = 0; t < secs; t += 0.1) {
        g.generate(x.data(), x.size());
        r.feed(x.data(), x.size());
        FsqTelemetry tt;
        if (r.telemetry(tt, seq)) { seq = tt.seq; res = std::move(tt); }
    }
    return res;
}

static const char* kBody = "LY2XYZ? snr please 73";

static Kbd33GenSignal sig(int speed, double centre, double snr, bool directed = true) {
    Kbd33GenSignal s;
    s.ifkp = false; s.speed = speed; s.centreHz = centre; s.snrDb = snr;
    s.text = directed ? fsqFrame("LY2ABC", kBody, true) : fsqFrame("LY2ABC", " cq cq cq de ly2abc kn54 k", false);
    s.pauseSec = 1.5; s.startSec = 1.0;
    return s;
}

// time to send the message twice, plus the time to find the signal
static double secsFor(int speed) { return 12 + 2.0 * 62 * kbd33::fsqSpec(speed).symSec + 3 * kbd33::fsqSpec(speed).symSec * 24 * 0.3; }

static bool hasMsg(const FsqTelemetry& t) {
    for (const auto& m : t.msgs) if (m.from == "LY2ABC" && m.crcOk && m.to == "LY2XYZ" && m.trigger == '?' && m.text == "snr please 73") return true;
    return false;
}

static void expect(const char* name, const FsqTelemetry& t, int speed) {
    const bool ok = hasMsg(t);
    printf("  %-24s speed %s centre %6.1f..%6.1f snr %5.1f q %.2f chars %llu drop %llu %s\n", name, t.speed >= 0 ? kbd33::speedName(false, t.speed) : "-", t.occLoHz, t.occHiHz, t.snrDb, t.quality, (unsigned long long)t.chars, (unsigned long long)t.blocksBad, ok ? "ok" : "MISS");
    CHECK(ok, "%s: message not found in %zu messages, text \"%s\" (state %d)", name, t.msgs.size(), t.text.c_str(), t.state);
    if (speed >= 0) CHECK(t.speed == speed, "%s: speed %d, wanted %d", name, t.speed, speed);
}

int main() {
    const ModeTuning mt = fsqTuning();
    CHECK(mt.stdMode == 115 && modeTuningById("fsq") && modeTuningById("fsq")->stdMode == 115, "tuning table");

    // The quick run leaves out cases another one covers: the clean runs but 3 baud (every speed decodes in "all together", with every error at once), a mirror of each
    // error (cfo -90 Hz, clock -250 ppm, drift +20 Hz/min), the second "all together" sign at each speed (the signs alternate), the noisy 1.5 / 3 baud, and
    // the call-sign and undirected-text checks run at 6 baud instead of 3 (the same text handling, a shorter message). ONAIR_TEST_LONG=1 runs every case.
    const bool L = testLong();
    FsqTelemetry clean2;   // clean 3 baud at 1500 Hz, 15 dB: the telemetry check at the end is the same run
    for (int sp = 0; sp < kbd33::kFsqSpeeds; sp++) {
        if (!L && sp != 2) continue;
        char nm[40]; snprintf(nm, sizeof nm, "clean %s baud", kbd33::speedName(false, sp));
        const FsqTelemetry t = run({sig(sp, 1500, 15)}, secsFor(sp));
        expect(nm, t, sp);
        if (sp == 2) clean2 = t;
    }
    for (int sp : {0, 2, 4}) {
        if (!L && sp != 4) continue;
        char nm[40]; snprintf(nm, sizeof nm, "noisy %s baud", kbd33::speedName(false, sp));
        expect(nm, run({sig(sp, 1500, 4)}, secsFor(sp) + 12), sp);
    }
    // the calling message from the user's view
    {
        const int csp = L ? 2 : 4;
        const FsqTelemetry t = run({sig(csp, 1500, 12)}, secsFor(csp), 0, -1, 0.02, 3, "LY2XYZ");
        bool toMe = false;
        for (const auto& m : t.msgs) if (m.to == "LY2XYZ" && m.toMe) toMe = true;
        CHECK(toMe, "the message to my call is marked");
        CHECK(!t.calls.empty() && t.calls[0].find("LY2") == 0, "callsigns found");
        const FsqTelemetry u = run({sig(csp, 1500, 12)}, secsFor(csp), 0, -1, 0.02, 3, "DL1AAA");
        bool any = false;
        for (const auto& m : u.msgs) if (m.toMe) any = true;
        CHECK(!any, "not marked for another call");
    }
    // the radio's tuning error, and where in the passband the band is
    expect("cfo +90 Hz", run({sig(2, 1500, 12)}, secsFor(2), 90), 2);
    if (L) expect("cfo -90 Hz", run({sig(2, 1500, 12)}, secsFor(2), -90), 2);
    expect("centre 600 Hz", run({sig(2, 600, 12)}, secsFor(2)), 2);
    if (L) expect("centre 3300 Hz", run({sig(2, 3300, 12)}, secsFor(2)), 2);
    expect("centre 3300 6 baud", run({sig(4, 3300, 12)}, secsFor(4)), 4);
    // every speed with a tuning error, a clock error and an off-centre band together
    for (int sp = 0; sp < kbd33::kFsqSpeeds; sp++) {
        for (int sgn : {1, -1}) {
            if (!L && sgn != (sp % 2 ? -1 : 1)) continue;
            auto s = sig(sp, sgn > 0 ? 2800 : 700, 10);
            s.clockPpm = 200 * sgn;
            s.driftHzPerMin = 10 * sgn;
            char nm[40]; snprintf(nm, sizeof nm, "all together %s %c", kbd33::speedName(false, sp), sgn > 0 ? '+' : '-');
            expect(nm, run({s}, secsFor(sp) + 6, 70 * sgn), sp);
        }
    }
    // the sender's clock
    {
        auto s = sig(2, 1500, 12); s.clockPpm = 250;
        expect("clock +250 ppm", run({s}, secsFor(2)), 2);
        s.clockPpm = -250;
        if (L) expect("clock -250 ppm", run({s}, secsFor(2)), 2);
        auto f = sig(4, 1500, 12); f.clockPpm = 300;
        expect("clock +300 ppm 6 baud", run({f}, secsFor(4)), 4);
    }
    // drift
    if (L) { auto s = sig(2, 1500, 12); s.driftHzPerMin = 20; expect("drift 20 Hz/min", run({s}, secsFor(2)), 2); }
    { auto s = sig(2, 1500, 12); s.driftHzPerMin = -30; expect("drift -30 Hz/min", run({s}, secsFor(2)), 2); }
    // fading and an echo
    { auto s = sig(2, 1500, 14); s.fadeDepth = 0.6; s.fadeHz = 0.15; expect("fading", run({s}, secsFor(2) + 20), 2); }
    { auto s = sig(2, 1500, 14); s.echoMs = 4; s.echoDb = -6; expect("echo 4 ms", run({s}, secsFor(2)), 2); }
    // a signal that starts late
    { auto s = sig(3, 1500, 12); s.startSec = 20; expect("starts at 20 s", run({s}, secsFor(3) + 20), 3); }
    // undirected text
    {
        const int usp = L ? 2 : 4;
        const FsqTelemetry t = run({sig(usp, 1500, 15, false)}, secsFor(usp) + 10);
        CHECK(t.text.find("cq cq cq de ly2abc kn54 k") != std::string::npos, "undirected text: \"%s\"", t.text.c_str());
        bool from = false;
        for (const auto& m : t.msgs) if (m.from == "LY2ABC" && m.crcOk && m.to.empty() && m.text.find("kn54") != std::string::npos) from = true;
        CHECK(from, "undirected line has its sender");
    }
    // a forced speed: the right one decodes, a wrong one does not
    expect("forced right speed", run({sig(3, 1500, 15)}, secsFor(3), 0, 3), 3);
    {
        const FsqTelemetry t = run({sig(3, 1500, 15)}, secsFor(3), 0, 1);
        CHECK(!hasMsg(t), "a wrong forced speed gave the message");
    }
    // nothing to decode
    {
        const FsqTelemetry n = run({}, 40);
        CHECK(n.chars == 0 && n.text.empty() && n.state == 0, "noise only: %llu chars, state %d", (unsigned long long)n.chars, n.state);
        Kbd33GenSignal k; k.ifkp = true; k.speed = 1; k.centreHz = 1500; k.snrDb = 12; k.text = "cq cq cq de ly2abc ifkp test k\n";
        const FsqTelemetry o = run({k}, 60);
        CHECK(!hasMsg(o) && o.text.find("ifkp") == std::string::npos, "an IFKP signal read as FSQ: \"%s\"", o.text.c_str());
    }
    // a second, weaker signal beside it does not spoil the first
    {
        auto a = sig(2, 1000, 14);
        auto b = sig(3, 2600, 8, false);
        expect("two signals", run({a, b}, secsFor(2) + 10), 2);
    }
    // telemetry that the app shows
    {
        const FsqTelemetry t = L ? run({sig(2, 1500, 15)}, secsFor(2)) : clean2;
        CHECK(t.state == 2 && t.dataValid && t.speed == 2 && std::fabs(t.baud - 2.93) < 0.05, "state %d speed %d baud %.2f", t.state, t.speed, t.baud);
        CHECK(!t.spectrum.empty() && t.toneUse.size() == kbd33::Rx::kPos && t.bandHz > 280 && t.bandHz < 300, "spectrum %zu tones %zu band %.0f", t.spectrum.size(), t.toneUse.size(), t.bandHz);
        CHECK(t.occHiHz > t.occLoHz && t.occHiHz - t.occLoHz < 300, "tones heard %.1f..%.1f", t.occLoHz, t.occHiHz);
        CHECK(t.snrDb > 8 && t.snrDb < 22, "SNR %.1f", t.snrDb);
    }
    return testSummary("fsq rx");
}
