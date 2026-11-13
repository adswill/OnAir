// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW skimmer against generated keyed carriers: several speeds at once across the window, two signals 150 Hz apart, a tuning error and
// signals near the window edges, QSB fading, weak signals in noise, hand keying with Farnsworth spacing, noise only (no false text),
// the focus on a picked frequency and its sidetone (through the tap only: silent).
#include "dect2/cw/cw_gen.h"
#include "dect2/cw/cw_rx.h"
#include "dect2/cw/cw_tel.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRate = 250000;
constexpr double kOff = -20000;   // the dial sits 20 kHz below the centre, as with the radio

struct Run { CwTelemetry t; double toneRms = 0; };

static Run run(const std::vector<CwGenSignal>& sigs, double secs, double cfo = 0, double focusHz = 1e9, bool tone = false, bool toneOn = true) {
    CwGenConfig c;
    c.rate = kRate; c.dialOffsetHz = kOff; c.cfoHz = cfo; c.signals = sigs; c.seed = 11;
    CwGenerator g(c);
    CwReceiver r;
    r.setSilent(true);
    r.configure(kRate);
    r.setSignalOffset(kOff);
    Run res;
    double e = 0; size_t ne = 0;
    if (tone) { r.setSidetone(toneOn); r.setSidetoneTap([&](const float* x, size_t n) { for (size_t i = 0; i < n; i++) e += (double)x[i] * x[i]; ne += n; }); }
    std::vector<cf32> x(25000);
    uint64_t seq = 0;
    for (double t = 0; t < secs; t += 0.1) {
        g.generate(x.data(), x.size());
        if (focusHz < 1e8 && t > 1.0 && t < 1.15) r.setFocusHz(focusHz);
        r.feed(x.data(), x.size());
        CwTelemetry tt;
        if (r.telemetry(tt, seq)) { seq = tt.seq; res.t = std::move(tt); }
    }
    res.toneRms = ne ? std::sqrt(e / ne) : 0;
    return res;
}

static const CwSignal* nearBy(const CwTelemetry& t, double hz, double tol = 30) {
    const CwSignal* b = nullptr;
    for (const auto& s : t.signals) if (std::fabs(s.offsetHz - hz) <= tol && (!b || s.chars > b->chars)) b = &s;
    return b;
}

static CwGenSignal sig(double hz, double wpm, const std::string& text, double snr) {
    CwGenSignal s;
    s.offsetHz = hz; s.wpm = wpm; s.text = text; s.snrDb = snr; s.pauseSec = 1.5;
    return s;
}

// the signal at hz must have sent want at least once, at its speed
static void expect(const char* name, const CwTelemetry& t, double hz, const std::string& want, double wpm) {
    const CwSignal* s = nearBy(t, hz);
    CHECK(s, "%s: no signal at %+.0f Hz (%zu signals)", name, hz, t.signals.size());
    if (!s) return;
    printf("  %-10s %+7.0f Hz %4.1f WPM %4.1f dB  \"%s\"\n", name, s->offsetHz, s->wpm, s->snrDb, s->text.size() > 70 ? s->text.substr(s->text.size() - 70).c_str() : s->text.c_str());
    CHECK(s->text.find(want) != std::string::npos, "%s %+.0f Hz: \"%s\" not in \"%s\"", name, hz, want.c_str(), s->text.c_str());
    CHECK(wpm <= 0 || std::fabs(s->wpm - wpm) < 0.12 * wpm, "%s %+.0f Hz: %.1f WPM, sent %.0f", name, hz, s->wpm, wpm);
}

int main() {
    const ModeTuning mt = cwTuning();
    CHECK(mt.stdMode == 36 && std::string(mt.id) == "cw", "tuning");
    int nb = 0;
    CHECK(cwBands(nb) && nb == 9, "bands");

    {   // five speeds at once across the window
        std::vector<CwGenSignal> s = {sig(-5200, 6, "CQ LY2ABC", 22), sig(-2600, 14, "CQ DE DL1XYZ K", 20), sig(300, 22, "TEST OK1XX TEST", 18),
                                      sig(2400, 35, "5NN TU G4ABC <SK>", 20), sig(5100, 50, "CQ TEST SP9ZZZ SP9ZZZ", 24)};
        const Run r = run(s, 48);
        expect("6 WPM", r.t, -5200, "CQ LY2ABC", 6);
        expect("14 WPM", r.t, -2600, "CQ DE DL1XYZ K", 14);
        expect("22 WPM", r.t, 300, "TEST OK1XX TEST", 22);
        expect("35 WPM", r.t, 2400, "5NN TU G4ABC <SK>", 35);
        expect("50 WPM", r.t, 5100, "CQ TEST SP9ZZZ SP9ZZZ", 50);
        CHECK(r.t.state == 2 && r.t.dataValid, "state %d", r.t.state);
    }
    {   // two signals 150 Hz apart, one 10 dB stronger
        std::vector<CwGenSignal> s = {sig(1000, 18, "CQ DE LY2ABC LY2ABC K", 28), sig(1150, 26, "TEST DL1XYZ DL1XYZ", 18)};
        const Run r = run(s, 30);
        expect("strong", r.t, 1000, "DE LY2ABC LY2ABC K", 18);
        expect("weak +150", r.t, 1150, "DL1XYZ DL1XYZ", 26);
    }
    {   // the radio 137 Hz off, signals near the window edges (they are heard where they are)
        std::vector<CwGenSignal> s = {sig(-6600, 20, "CQ DE 9A1AA K", 20), sig(6500, 24, "CQ DE W1AW K", 20)};
        const Run r = run(s, 24, 137);
        expect("edge -", r.t, -6600 + 137, "CQ DE 9A1AA K", 20);
        expect("edge +", r.t, 6500 + 137, "CQ DE W1AW K", 24);
    }
    {   // QSB: 20 dB deep over 6 s, and a drifting transmitter
        CwGenSignal a = sig(-1500, 20, "CQ CQ DE LY2ABC LY2ABC <AR> K", 30);
        a.qsbDepthDb = 20; a.qsbPeriodSec = 6;
        CwGenSignal b = sig(1800, 16, "CQ DE OH2XYZ K", 22);
        b.driftHzPerMin = 40;
        const Run r = run({a, b}, 40);
        expect("QSB", r.t, -1500, "DE LY2ABC LY2ABC", 20);
        expect("drift", r.t, 1800 + 40 * 40 / 60.0, "CQ DE OH2XYZ K", 16);
    }
    {   // weak: 8 dB in 500 Hz
        const Run r = run({sig(-800, 18, "CQ DE LY2ABC LY2ABC K", 8)}, 36);
        expect("weak", r.t, -800, "LY2ABC", 18);
    }
    {   // hand keyed: 12 % jitter, heavy dashes, Farnsworth 18 / 10
        CwGenSignal a = sig(700, 18, "LY2ABC DE G4ABC UR 599 599 <BT> NAME JOHN <KN>", 22);
        a.jitter = 0.12; a.dashRatio = 3.5; a.farnsworthWpm = 10;
        const Run r = run({a}, 60);
        expect("hand", r.t, 700, "UR 599 599", 18);
        const CwSignal* s = nearBy(r.t, 700);
        CHECK(s && s->farnsworth, "Farnsworth flag");
        CHECK(s && s->calls.size() >= 2, "callsigns: %zu", s ? s->calls.size() : 0);
    }
    {   // noise only: no text
        const Run r = run({}, 30);
        uint64_t chars = 0;
        for (const auto& s : r.t.signals) chars += s.chars;
        CHECK(chars <= 2, "noise only: %llu characters from %zu signals", (unsigned long long)chars, r.t.signals.size());
    }
    {   // the focus on a picked frequency, and its sidetone
        const Run r = run({sig(-2000, 20, "CQ DE LY2ABC K", 25), sig(1200, 25, "TEST DL1XYZ", 25)}, 14, 0, 1210, true);
        const CwSignal* f = cwFocus(r.t);
        CHECK(f && std::fabs(f->offsetHz - 1200) < 30, "focus %+.0f", f ? f->offsetHz : 0.0);
        CHECK(r.toneRms > 0.02, "sidetone rms %.3f", r.toneRms);
        const Run q = run({sig(1200, 25, "TEST DL1XYZ", 25)}, 8, 0, 1210, true, false);
        CHECK(q.toneRms == 0, "sidetone off: audio rms %.3f", q.toneRms);
    }
    printf(fails ? "cw rx: %d FAILED\n" : "cw rx: all passed\n", fails);
    return fails ? 1 : 0;
}
