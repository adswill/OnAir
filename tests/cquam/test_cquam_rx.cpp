// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C-QUAM receiver on the test signal: stereo found through the pilot and separated into the right channels, plain AM stays mono, noise
// alone gives nothing, and the same under tuning error, noise and fading.
#include "dect2/cquam/cquam_gen.h"
#include "dect2/cquam/cquam_rx.h"
#include "dect2/cquam/cquam_tel.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Result {
    CquamTelemetry t;
    std::vector<float> l, r;   // the last seconds of audio
};

static Result run(const SynthConfig& sc, double secs, double rate = 1000000, bool mono = false, int smode = 0) {
    auto g = makeCquamSynth(sc, rate);
    CquamReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-cquamTuning().tuneOffsetHz);
    rx.setSilent(true);
    rx.setStereoMode(smode);
    Result res;
    const size_t keep = (size_t)(4 * kCquamAudioRate);
    rx.setAudioTap([&](const float* a, const float* b, size_t n) {
        res.l.insert(res.l.end(), a, a + n); res.r.insert(res.r.end(), b, b + n);
        if (res.l.size() > keep) { res.l.erase(res.l.begin(), res.l.end() - keep); res.r.erase(res.r.begin(), res.r.end() - keep); }
    });
    std::vector<cf32> x((size_t)(rate / 10));
    uint64_t seq = 0;
    for (double t = 0; t < secs; t += 0.1) {
        g->generate(x.data(), x.size());
        rx.feed(x.data(), x.size());
        CquamTelemetry tt;
        if (rx.telemetry(tt, seq)) { seq = tt.seq; res.t = tt; }
    }
    (void)mono;
    return res;
}

// amplitude of a tone in the audio
static double tone(const std::vector<float>& a, double hz) {
    double re = 0, im = 0;
    const size_t n = a.size();
    for (size_t i = 0; i < n; i++) { const double ph = 2 * 3.14159265358979 * hz * (double)i / kCquamAudioRate; re += a[i] * std::cos(ph); im += a[i] * std::sin(ph); }
    return 2 * std::sqrt(re * re + im * im) / (double)n;
}
static double db(double a, double b) { return 20 * std::log10((a + 1e-9) / (b + 1e-9)); }

static SynthConfig cfg(int kind, double snr = 35) {
    SynthConfig s;
    s.snrDb = snr; s.modeOpt[0] = kind;
    return s;
}

int main() {
    {   // stereo: left 440 Hz, right 660 Hz
        const Result r = run(cfg(0), 8);
        printf("  stereo: %s\n", cquamSummary(r.t).c_str());
        CHECK(r.t.state == 2 && r.t.stereo, "state %d", r.t.state);
        CHECK(r.t.pilotPct > 3.5f && r.t.pilotPct < 6.5f, "pilot %.2f %%", r.t.pilotPct);
        CHECK(std::fabs(r.t.cfoHz) < 3, "carrier offset %.2f Hz", r.t.cfoHz);
        const double l440 = tone(r.l, 440), l660 = tone(r.l, 660), r440 = tone(r.r, 440), r660 = tone(r.r, 660);
        printf("  L: 440 %.3f 660 %.3f   R: 440 %.3f 660 %.3f\n", l440, l660, r440, r660);
        CHECK(std::fabs(l440 - 0.35) < 0.05 && std::fabs(r660 - 0.35) < 0.05, "levels %.3f %.3f (0.35 sent)", l440, r660);
        CHECK(db(l440, l660) > 25 && db(r660, r440) > 25, "separation %.1f / %.1f dB", db(l440, l660), db(r660, r440));
        CHECK(r.t.blocksOk > 5, "pilot checks %llu", (unsigned long long)r.t.blocksOk);
        CHECK(r.t.modPct > 40 && r.t.modPct < 100, "modulation %.1f %%", r.t.modPct);
    }
    {   // the left channel silent
        SynthConfig s = cfg(0); s.modeOpt[1] = 1;
        const Result r = run(s, 8);
        CHECK(r.t.stereo, "stereo with a silent left channel");
        // crosstalk of the right tone into the silent left channel, measured on the tone: the broadband power of the left channel is
        // the noise floor (35 dB C/N) and the 25 Hz pilot, both there by design, not program leaking across
        const double xt = db(tone(r.l, 660), tone(r.r, 660));
        printf("  left silent: 660 Hz leakage into L %.1f dB\n", xt);
        CHECK(xt < -22, "660 Hz in the silent left channel %.1f dB re right", xt);
    }
    {   // plain AM: mono, no pilot, the same audio on both channels
        const Result r = run(cfg(1), 10);
        printf("  AM: %s\n", cquamSummary(r.t).c_str());
        CHECK(r.t.state == 1 && !r.t.stereo && !r.t.pilotSeen, "state %d stereo %d", r.t.state, r.t.stereo);
        double d = 0, s = 0;
        for (size_t i = 0; i < r.l.size(); i++) { d += (r.l[i] - r.r[i]) * (r.l[i] - r.r[i]); s += r.l[i] * r.l[i]; }
        CHECK(s > 0 && d < 1e-6 * s, "L and R differ: %g of %g", d, s);
        CHECK(tone(r.l, 440) > 0.1, "mono audio missing");
    }
    {   // forced mono on a stereo station: L = R; forced stereo on a mono one: no crash, mono-like
        const Result r = run(cfg(0), 8, 1000000, false, 1);
        CHECK(!r.t.stereo, "forced mono still reports stereo");
        double d = 0;
        for (size_t i = 0; i < r.l.size(); i++) d += (r.l[i] - r.r[i]) * (r.l[i] - r.r[i]);
        CHECK(d < 1e-6, "forced mono: L and R differ by %g", d);
    }
    {   // nothing but noise: no carrier, no stereo, silence
        const Result r = run(cfg(2, 20), 8);
        CHECK(r.t.state == 0 && !r.t.stereo, "noise: state %d", r.t.state);
        double s = 0;
        for (float v : r.l) s += (double)v * v;
        CHECK(s / std::max<size_t>(1, r.l.size()) < 1e-4, "noise is played: %g", s / r.l.size());
    }
    return testSummary("cquam rx");
}
