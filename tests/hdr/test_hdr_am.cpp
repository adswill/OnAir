// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HD Radio AM: the hybrid (MA1) and all-digital (MA3) waveforms through the receiver at the rates the HF radios give (200 ksps, an Airspy
// HF+ at 912 and 768 ksps, 1 Msps, 2 Msps), with the faults of medium wave: small tuning errors (+-10 to +-100 Hz), +-100 ppm sample
// clocks, and the slow deep fading of the sky wave at night (the amplitude swinging by 12 dB a few times a minute, and dips of 20 dB).
// Every case must find the waveform, the service mode, the station information (call sign, name, slogan, message) and the program data
// (title and artist) of both programs, as on FM. MA3 also carries P3 (HD1's enhanced stream, on the weaker secondary and tertiary
// sidebands), which must decode when the signal is good (25 dB).
#include "dect2/hdr/hdr_gen.h"
#include "dect2/hdr/hdr_rx.h"
#include "../common/impair.h"
#include "../common/jobs.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

// mode: 1 MA1, 3 MA3 (SynthConfig::modeOpt[0])
static std::vector<cf32> make(int mode, double rate, double secs, double snrDb) {
    SynthConfig sc;
    sc.mode = 23; sc.modeOpt[0] = mode; sc.snrDb = snrDb;
    auto syn = makeHdrSynth(sc, rate);
    std::vector<cf32> x((size_t)(secs * rate));
    syn->generate(x.data(), x.size());
    return x;
}

struct Fade { double depthDb = 0, periodSec = 0; double dipDb = 0, dipEvery = 0, dipLen = 0; };

// The signal at 25 dB SNR (at the top of the fades) with the fading on the signal only (the noise of the receiver stays where it is): the
// generator is deterministic, so the noise is the difference of a 25 dB and a noise-free run.
static std::vector<cf32> faded(int mode, double rate, double secs, const Fade& f) {
    std::vector<cf32> s = make(mode, rate, secs, 100), y = make(mode, rate, secs, 25);
    for (size_t i = 0; i < s.size(); i++) {
        const double t = (double)i / rate;
        double db = -0.5 * f.depthDb * (1 - std::cos(2 * M_PI * t / f.periodSec));
        if (f.dipEvery > 0) {
            const double ph = std::fmod(t, f.dipEvery);
            if (ph < f.dipLen) db -= f.dipDb * std::sin(M_PI * ph / f.dipLen);
        }
        const float g = (float)std::pow(10.0, db / 20);
        y[i] = g * s[i] + (y[i] - s[i]);
    }
    return y;
}

static void check(const char* what, int mode, double rate, const std::vector<cf32>& x, double cfo, unsigned maxBad, bool p3 = false) {
    const HdrTestContent& tc = hdrTestContent();
    HdrReceiver rx;
    rx.configure(rate);
    CHECK(rx.ready(), "%s: not ready at %.0f sps", what, rate);
    for (size_t i = 0; i < x.size(); i += 20000) rx.feed(x.data() + i, std::min<size_t>(20000, x.size() - i));
    HdrTelemetry t;
    rx.telemetry(t, 0);
    int psd = 0;
    for (const auto& p : t.programs)
        for (const auto& c : tc.programs) if (p.number == c.number && p.title == c.title && p.artist == c.artist) psd++;
    jprintf("%-46s %s, CFO %+6.1f Hz, MER %4.1f dB, P1 %llu ok %llu bad, P3 %llu ok, '%s', %d of 2 programs' data\n", what, t.modeName.c_str(), t.cfoHz, t.snrDb,
            (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.p3Ok, t.callSign.c_str(), psd);
    CHECK(t.state == 3 && t.band == 2 && t.serviceMode == (mode == 3 ? 2 : 1), "%s: state %d band %d mode %d", what, t.state, t.band, t.serviceMode);
    CHECK(t.callSign == tc.callSign && t.stationName == tc.name && t.slogan == tc.slogan && t.message == tc.message, "%s: station information", what);
    CHECK(psd == 2, "%s: program data of %d programs", what, psd);
    CHECK(t.blocksOk >= 10 && t.blocksBad <= maxBad, "%s: P1 %llu ok %llu bad", what, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    if (p3) CHECK(t.p3Ok >= 3 && t.p3Bad == 0, "%s: P3 %llu ok %llu bad", what, (unsigned long long)t.p3Ok, (unsigned long long)t.p3Bad);
    if (!std::isnan(cfo)) CHECK(std::fabs(t.cfoHz - cfo) < 10, "%s: CFO %.1f, sent %.1f", what, t.cfoHz, cfo);
}

int main() {
    testjobs::Jobs jobs;
    const double secs = 18;
    // the rates of the HF radios, both waveforms
    for (int m : {1, 3})
        for (double r : {200000.0, 768000.0, 912000.0, 2000000.0}) jobs.add([=] {
            char b[80]; snprintf(b, sizeof b, "%s at %.0f ksps", m == 3 ? "MA3" : "MA1", r / 1000);
            check(b, m, r, make(m, r, secs, 15), 0, 1);
        });
    for (int m : {1, 3}) jobs.add([=] {   // P3 needs a good signal (MA1: 32 dB, tests/test_hdr_rx.cpp)
        check(m == 3 ? "MA3 at 25 dB, P3" : "MA1 at 34 dB, P3", m, 912000, make(m, 912000, secs, m == 3 ? 25 : 34), 0, 0, true);
    });
    // tuning errors of a medium wave receiver
    for (int m : {1, 3})
        for (double hz : {10.0, -10.0, 50.0, -50.0, 100.0, -100.0}) jobs.add([=] {
            const double r = 912000;
            auto x = make(m, r, secs, 15);
            impair::shift(x, hz, r);
            char b[80]; snprintf(b, sizeof b, "%s, tuning error %+.0f Hz", m == 3 ? "MA3" : "MA1", hz);
            check(b, m, r, x, hz, 1);
        });
    // sample clocks 100 ppm off (with a small offset as well)
    for (int m : {1, 3})
        for (double ppm : {100.0, -100.0}) jobs.add([=] {
            const double r = 1000000;
            auto x = impair::clock(make(m, r, secs, 15), ppm);
            impair::shift(x, 40, r);
            char b[80]; snprintf(b, sizeof b, "%s, sample clock %+.0f ppm", m == 3 ? "MA3" : "MA1", ppm);
            check(b, m, r, x, std::nan(""), 1);
        });
    // sky wave fading (25 dB SNR at the top): the level swinging by 12 dB every 7 s, and 20 dB dips of 0.6 s every 5 s on top. The frames
    // in the dips are lost; the receiver must hold on (or find the station again) and read everything in between.
    for (int m : {1, 3}) {
        jobs.add([=] {
            Fade f; f.depthDb = 12; f.periodSec = 7;
            check(m == 3 ? "MA3, slow fading 12 dB" : "MA1, slow fading 12 dB", m, 912000, faded(m, 912000, 22, f), 0, 4);
        });
        jobs.add([=] {
            Fade f; f.depthDb = 6; f.periodSec = 3; f.dipDb = 20; f.dipEvery = 5; f.dipLen = 0.6;
            check(m == 3 ? "MA3, fading with 20 dB dips" : "MA1, fading with 20 dB dips", m, 912000, faded(m, 912000, 22, f), 0, 12);
        });
    }
    // all of it at once: an HF+ at 768 ksps, -70 Hz, -100 ppm, fading
    for (int m : {1, 3}) jobs.add([=] {
        Fade f; f.depthDb = 10; f.periodSec = 6;
        auto x = impair::clock(faded(m, 768000, 22, f), -100);
        impair::shift(x, -70, 768000);
        check(m == 3 ? "MA3 combined: 768 ksps, -70 Hz, -100 ppm, fading" : "MA1 combined: 768 ksps, -70 Hz, -100 ppm, fading", m, 768000, x, -70, 4);
    });
    // 3.9 acquisition (docs/SENSITIVITY-3.9-E.md): MA1 at 30 dB, no tuning error, started at several points of the signal: the first P1 frame
    // within 10 s every time (sync after a few blocks plus the diversity delay of about 6 s). 3.8 switched the prefix filter in the middle of
    // reading the block numbers and needed 11 s for one of these starts, and in a sens_sweep trial never locked at all.
    for (double startSec : {0.0, 1.0, 4.0, 5.2}) jobs.add([=] {
        const double r = 250000;
        std::vector<cf32> x = make(1, r, 16 + startSec, 30);
        x.erase(x.begin(), x.begin() + (long)(startSec * r));
        HdrReceiver rx;
        rx.configure(r);
        double lockS = -1;
        uint64_t seq = 0;
        HdrTelemetry t;
        for (size_t i = 0; i < x.size() && lockS < 0; i += 20000) {
            rx.feed(x.data() + i, std::min<size_t>(20000, x.size() - i));
            if (rx.telemetry(t, seq)) { seq = t.seq; if (t.blocksOk > 0) lockS = (double)(i + 20000) / r; }
        }
        jprintf("MA1 at 30 dB, started %.1f s into the signal: first P1 frame after %.2f s\n", startSec, lockS);
        CHECK(lockS > 0 && lockS < 10.0, "start %.1f s: first P1 frame after %.2f s", startSec, lockS);
    });
    jobs.run();
    if (fails) { printf("%d check(s) failed\n", fails.load()); return 1; }
    printf("OK\n");
    return 0;
}
