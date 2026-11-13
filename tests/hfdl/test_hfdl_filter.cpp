// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Baseband filter (dsp/baseband_filter.h): the default design is the HFDL receiver's original filter bit for bit, the response of every shape
// (pass-band, stop-band, centre offset), the text form, and HFDL decoding through every preset with tuning errors and a strong adjacent channel.
#include "dect2/dsp/baseband_filter.h"
#include "dect2/hfdl/hfdl_gen.h"
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/dsp/portable_rng.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 48000, kCentre = 8.935e6, kCh0 = 8.927e6, kCh1 = 8.942e6;

static std::vector<float> legacy(int taps, double fc, double fs) {
    std::vector<float> h((size_t)taps);
    double s = 0;
    for (int i = 0; i < taps; i++) {
        const double m = i - (taps - 1) / 2.0, x = 2 * fc / fs * m;
        const double sinc = std::fabs(m) < 1e-9 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (taps - 1)) + 0.08 * std::cos(4 * M_PI * i / (taps - 1));
        h[(size_t)i] = (float)(2 * fc / fs * sinc * w);
        s += h[(size_t)i];
    }
    for (auto& v : h) v = (float)(v / s);
    return h;
}

// one data burst on channel 0 at tuning error cfo, a strong squitter-like burst train on the other channel; returns ACARS messages decoded on channel 0
static int run(const BasebandFilterCfg& cfg, double cfo, double adjacentDb, int m1) {
    HfdlReceiver rx;
    rx.configure(kRate);
    rx.setCenterHz(kCentre);
    rx.setBasebandFilter(cfg);
    rx.setChannels({kCh0, kCh1});
    const std::vector<uint8_t> pdu = hfdlMpduDown(1, 23, {hfdlUnnumbered(hfdlAcarsHfnpdu("N1UA", "H1", '2', "FILTER TEST", true))});
    HfdlWave w = hfdlMakeWave(m1, pdu, {}, 5);
    w.start = 0.3 * kRate; w.carHz = kCh0 + kHfdlCarrierHz - kCentre + cfo; w.amp = 0.1f; w.phase = 0.3;
    HfdlSquitterSpec sp; sp.gsId = 1; sp.frameIndex = 5; sp.mask = 0x21; sp.other[0] = {7, true, 3};
    HfdlWave a = hfdlMakeWave(1, hfdlSpdu(sp), {}, 6);
    a.start = 0.2 * kRate; a.carHz = kCh1 + kHfdlCarrierHz - kCentre; a.amp = (float)(0.1 * std::pow(10.0, adjacentDb / 20)); a.phase = 1.1;
    PortableRng rng(3);
    const double sigma = std::sqrt(0.1 * 0.1 / std::pow(10.0, 25 / 10.0) * (kRate / kHfdlBaud) / 2);
    std::vector<cf32> buf(4096);
    const double secs = 0.3 + hfdlBurstSeconds(m1) + 1.0;
    for (int64_t pos = 0; pos < (int64_t)(secs * kRate); pos += 4096) {
        for (auto& v : buf) v = cf32((float)rng.gauss(sigma), (float)rng.gauss(sigma));
        hfdlRender(w, buf.data(), pos, 4096, kRate);
        hfdlRender(a, buf.data(), pos, 4096, kRate);
        rx.feed(buf.data(), 4096);
    }
    HfdlTelemetry t;
    rx.telemetry(t, ~0ull);
    int n = 0;
    for (const auto& m : t.messages) n += m.acars && m.acarsCrcOk && m.freqHz == kCh0;
    return n;
}

int main() {
    // the default design is the original filter
    for (double fs : {48000.0, 28800.0, 24000.0}) {
        const int taps = std::max(31, std::min(301, (int)std::ceil(4.0 * fs / 2000) | 1));
        const std::vector<float> ref = legacy(taps, 1700, fs);
        const BbDesign d = bbf::design(BasebandFilterCfg{}, 3400, 2000, fs);
        CHECK(d.h == ref && !d.shifted, "default taps differ from the original at %.0f Hz", fs);
    }
    // responses at 28.8 kHz
    const double fs = 28800;
    for (int s = 0; s < 5; s++) {
        BasebandFilterCfg c{true, 3000, (BbShape)s, 1500, 0};
        const BbDesign d = bbf::design(c, 3400, 2000, fs);
        const double dc = bbf::responseDb(d, fs, 0), edge = bbf::responseDb(d, fs, 800), cut = bbf::responseDb(d, fs, 1500), stop = bbf::responseDb(d, fs, 4500);
        printf("  %-16s taps %3zu: dc %+.2f, 0.8k %+.2f, cut %+.2f, 4.5k %+.1f dB\n", bbf::shapeName((BbShape)s), d.h.size(), dc, edge, cut, stop);
        CHECK(std::fabs(dc) < 0.1, "%s dc %f", bbf::shapeName((BbShape)s), dc);
        CHECK(edge > -1.5, "%s pass-band edge %f", bbf::shapeName((BbShape)s), edge);
        CHECK(cut < -2.0 && cut > -9.0, "%s cut-off %f", bbf::shapeName((BbShape)s), cut);
        CHECK(stop < (s == 3 || s == 4 ? -30 : -40), "%s stop-band %f", bbf::shapeName((BbShape)s), stop);
    }
    {   // centre offset moves the pass-band
        BasebandFilterCfg c{true, 2000, BbShape::Kaiser, 600, 1000};
        const BbDesign d = bbf::design(c, 3400, 2000, fs);
        CHECK(d.shifted && bbf::responseDb(d, fs, 1000) > -0.5 && bbf::responseDb(d, fs, -1000) < -20, "centre offset response");
    }
    {   // text form
        BasebandFilterCfg c{true, 2800, BbShape::BlackmanHarris, 1200, -150};
        CHECK(bbf::fromString(bbf::toString(c)) == c, "round trip");
        CHECK(!bbf::fromString("junk").enabled, "junk is the default");
    }
    {   // the per-mode registry: nothing set returns the mode's own taps untouched; switched on, the chosen width
        const std::vector<float> own = bbf::lowpass(BbShape::Kaiser, 3000, 500, 48000);
        CHECK(bbf::overrideTaps("airband", own, 48000, 3000.0) == own, "default taps are the mode's own");
        bbf::set("airband", BasebandFilterCfg{true, 1200, BbShape::Hann, 400, 0});
        const std::vector<float> mine = bbf::overrideTaps("airband", own, 48000, 3000.0);
        BbDesign d; d.h = mine;
        CHECK(mine != own && bbf::responseDb(d, 48000, 300) > -1 && bbf::responseDb(d, 48000, 1500) < -20, "custom width");
        CHECK(bbf::overrideTaps("vdl2", own, 48000, 3000.0) == own, "other keys unaffected");
        bbf::set("airband", BasebandFilterCfg{});
        CHECK(bbf::overrideTaps("airband", own, 48000, 3000.0) == own, "switched off again");
        for (int f : {14, 16, 23, 24, 25, 26, 27, 34, 35, 36, 55}) CHECK(*bbf::keyForFamily(f), "family %d has a key", f);
        CHECK(!*bbf::keyForFamily(0), "DVB has none");
    }
    // HFDL decode through every preset, off-tune and next to a strong channel
    int total = 0, ok = 0;
    for (const auto& p : bbf::presets()) {
        if (std::strcmp(p.mode, "hfdl") != 0) continue;
        for (double cfo : {-200.0, 0.0, 200.0}) for (int m1 : {1, 2}) {
            const int n = run(p.cfg, cfo, 12, m1);
            total++; ok += n == 1;
            CHECK(n == 1, "%s cfo %+.0f m1 %d: %d messages", p.name, cfo, m1, n);
        }
    }
    printf("  preset sweep: %d of %d decoded\n", ok, total);
    CHECK(run(BasebandFilterCfg{}, 100, 12, 1) == 1, "default settings");
    return testSummary("hfdl filter");
}
