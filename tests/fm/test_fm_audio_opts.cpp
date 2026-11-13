// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The FM sound options of issue #213 on the test transmitter (fm_gen.h): de-emphasis off keeps a 10 kHz tone that 50 us cuts, the 5 kHz
// audio bandwidth removes a 10 kHz tone and passes 1 kHz, the squelch mutes noise and opens on a station, and the stereo choice (auto,
// mono, stereo with a weak pilot) changes what reaches the right channel.
// NOT covered: a real station, the sound device.
#include "dect2/fm/fm_gen.h"
#include "dect2/fm/fm_rx.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Run { FmTelemetry tel; std::vector<float> l, r; };

static Run run(const FmGenConfig& cfg, double secs, const std::function<void(FmReceiver&)>& set, bool noiseOnly = false) {
    Run out;
    FmGenerator gen(cfg);
    FmReceiver rx;
    rx.setSilent(true);
    rx.configure(cfg.rate);
    set(rx);
    rx.setAudioTap([&](const float* l, const float* r, size_t n) { out.l.insert(out.l.end(), l, l + n); out.r.insert(out.r.end(), r, r + n); });
    std::vector<cf32> buf;
    std::mt19937 rng(5);
    std::normal_distribution<float> g(0.f, 0.1f);
    const size_t block = 65536, total = (size_t)(secs * cfg.rate);
    uint64_t seq = 0;
    for (size_t done = 0; done < total; done += block) {
        buf.clear();
        const size_t n = std::min(block, total - done);
        if (noiseOnly) for (size_t i = 0; i < n; i++) buf.push_back(cf32(g(rng), g(rng)));
        else gen.generate(n, buf);
        rx.feed(buf.data(), buf.size());
        FmTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; out.tel = t; }
    }
    return out;
}

// the amplitude of a tone in the last `secs` of x (48 kHz)
static double toneAmp(const std::vector<float>& x, double hz, double secs) {
    const size_t n = std::min(x.size(), (size_t)(secs * 48000));
    double re = 0, im = 0;
    for (size_t i = 0; i < n; i++) {
        const double ph = 2 * M_PI * hz * (double)i / 48000.0, v = x[x.size() - n + i];
        re += v * std::cos(ph); im += v * std::sin(ph);
    }
    return n ? 2 * std::sqrt(re * re + im * im) / n : 0;
}
static double rms(const std::vector<float>& x, double secs) {
    const size_t n = std::min(x.size(), (size_t)(secs * 48000));
    double s = 0;
    for (size_t i = 0; i < n; i++) s += (double)x[x.size() - n + i] * x[x.size() - n + i];
    return n ? std::sqrt(s / n) : 0;
}
static double db(double a, double b) { return 20 * std::log10(std::max(a, 1e-9) / std::max(b, 1e-9)); }

int main() {
    FmGenConfig c;
    c.rate = 1e6; c.cnrDb = 50; c.stereo = false; c.rds = false; c.preemphUs = 0; c.leftHz = 10000; c.rightHz = 10000; c.leftAmp = c.rightAmp = 0.3f;
    // de-emphasis: a 10 kHz tone sent without pre-emphasis is cut by 50 us (about 10 dB) and kept with de-emphasis off
    const Run de50 = run(c, 1.5, [](FmReceiver& r) { r.setDeemphasis(50); });
    const Run deOff = run(c, 1.5, [](FmReceiver& r) { r.setDeemphasis(0); });
    const double a50 = toneAmp(de50.l, 10000, 0.8), aOff = toneAmp(deOff.l, 10000, 0.8);
    printf("10 kHz tone: de-emphasis 50 us %.4f, off %.4f (%.1f dB)\n", a50, aOff, db(aOff, a50));
    CHECK(db(aOff, a50) > 6 && aOff > 0.05, "de-emphasis off keeps the treble: %.1f dB", db(aOff, a50));

    // audio bandwidth: 5 kHz removes a 10 kHz tone, passes 1 kHz
    const Run full = run(c, 1.5, [](FmReceiver& r) { r.setDeemphasis(0); r.setAudioBandwidth(0); });
    const Run bw5 = run(c, 1.5, [](FmReceiver& r) { r.setDeemphasis(0); r.setAudioBandwidth(5000); });
    const double cut = db(toneAmp(full.l, 10000, 0.8), toneAmp(bw5.l, 10000, 0.8));
    FmGenConfig lo = c; lo.leftHz = lo.rightHz = 1000;
    const Run lfull = run(lo, 1.5, [](FmReceiver& r) { r.setDeemphasis(0); });
    const Run lbw5 = run(lo, 1.5, [](FmReceiver& r) { r.setDeemphasis(0); r.setAudioBandwidth(5000); });
    const double pass = db(toneAmp(lfull.l, 1000, 0.8), toneAmp(lbw5.l, 1000, 0.8));
    printf("5 kHz bandwidth: 10 kHz down %.1f dB, 1 kHz down %.2f dB\n", cut, pass);
    CHECK(cut > 20, "10 kHz cut by %.1f dB", cut);
    CHECK(std::fabs(pass) < 1.0, "1 kHz changed by %.2f dB", pass);

    // squelch: noise alone stays silent, a station opens it
    const Run sqNoise = run(c, 1.5, [](FmReceiver& r) { r.setSquelch(15); }, true);
    const Run noSq = run(c, 1.5, [](FmReceiver&) {}, true);
    const Run sqOpen = run(c, 1.5, [](FmReceiver& r) { r.setSquelch(15); r.setDeemphasis(0); });
    printf("squelch: noise %.5f (without squelch %.5f, squelched %d), station %.4f\n", rms(sqNoise.l, 0.5), rms(noSq.l, 0.5), sqNoise.tel.squelched, rms(sqOpen.l, 0.5));
    CHECK(rms(sqNoise.l, 0.5) < 1e-4 && rms(noSq.l, 0.5) > 1e-3 && sqNoise.tel.squelched, "the squelch mutes noise");
    CHECK(rms(sqOpen.l, 0.5) > 0.05 && !sqOpen.tel.squelched, "the squelch opens on a station");

    // stereo: left only. Auto keeps the right channel quiet, mono puts the left tone in both
    FmGenConfig st = c; st.stereo = true; st.leftHz = 1000; st.rightHz = 1000; st.leftAmp = 0.4f; st.rightAmp = 0.f; st.preemphUs = 50;
    const Run sAuto = run(st, 2.0, [](FmReceiver& r) { r.setStereoMode(0); });
    const Run sMono = run(st, 2.0, [](FmReceiver& r) { r.setStereoMode(1); });
    const double sepAuto = db(toneAmp(sAuto.l, 1000, 0.8), toneAmp(sAuto.r, 1000, 0.8)), sepMono = db(toneAmp(sMono.l, 1000, 0.8), toneAmp(sMono.r, 1000, 0.8));
    printf("stereo: auto separation %.1f dB (stereo %d), mono %.1f dB (stereo %d)\n", sepAuto, sAuto.tel.stereo, sepMono, sMono.tel.stereo);
    CHECK(sAuto.tel.stereo && sepAuto > 20, "auto decodes stereo");
    CHECK(!sMono.tel.stereo && std::fabs(sepMono) < 1, "mono puts the left tone in both channels");
    // a weak pilot (1 %): auto stays mono, forced stereo separates
    FmGenConfig weak = st; weak.pilotPct = 1.0;
    const Run wAuto = run(weak, 2.0, [](FmReceiver& r) { r.setStereoMode(0); });
    const Run wForce = run(weak, 2.0, [](FmReceiver& r) { r.setStereoMode(2); });
    const double sepWA = db(toneAmp(wAuto.l, 1000, 0.8), toneAmp(wAuto.r, 1000, 0.8)), sepWF = db(toneAmp(wForce.l, 1000, 0.8), toneAmp(wForce.r, 1000, 0.8));
    printf("weak pilot: auto %.1f dB (stereo %d), forced %.1f dB (stereo %d)\n", sepWA, wAuto.tel.stereo, sepWF, wForce.tel.stereo);
    CHECK(sepWF > sepWA + 6 && wForce.tel.stereo, "forced stereo separates on a weak pilot");
    return testSummary("fm audio options");
}
