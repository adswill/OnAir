// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Auto for the DTMB channel width: 8 MHz (7.56 Msym/s) against 6 MHz (5.67), measured from the spectrum. Simulated signals only (dtmb_gen).
#include "dect2/dtmb/dtmb_testkit.h"
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::dtmb;
using namespace dect2::dtmb::kit;
#include "../common/test_util.h"

namespace {
struct R { double width; int state; };
R run(double symRate, double snr, double cfo, double seconds, bool withWrongStart, uint32_t seed) {
    constexpr double kRate = 10e6;
    SignalConfig sc = makeSignal(Header::Pn945, Mapping::Qam64, Rate::R06, false, kRate, snr);
    sc.symbolRate = symRate; sc.cfoHz = cfo;
    Signal sig(sc, testPacketSource(seed));
    DtmbReceiver rx;
    rx.configure(kRate, withWrongStart ? (symRate < 7e6 ? 8 : 6) : (symRate < 7e6 ? 6 : 8));
    rx.setDecoderThreads(0);
    rx.setAutoWidth(true);
    std::vector<cf32> buf(40000);
    const size_t total = (size_t)(seconds * kRate);
    DtmbTelemetry t;
    uint64_t seq = 0;
    for (size_t i = 0; i < total; i += buf.size()) {
        sig.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        DtmbTelemetry nt;
        if (rx.telemetry(nt, seq)) { seq = nt.seq; t = nt; }
    }
    return {t.autoWidthMhz, t.state};
}
}

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    int ok = 0, n = 0;
    for (double sr : {7.56e6, 5.67e6}) for (double snr : {30.0, 20.0, 12.0, 6.0}) for (double cfo : {0.0, 40e3, -90e3}) {
        const R r = run(sr, snr, cfo, 1.0, true, (uint32_t)(n + 3));
        n++;
        const double want = sr < 7e6 ? 6 : 8;
        ok += r.width == want;
        if (r.width != want) printf("  sr %.2f snr %.0f cfo %.0f -> %.0f\n", sr / 1e6, snr, cfo, r.width);
    }
    printf("DTMB width, both widths x SNR 30/20/12/6 dB x offsets 0/40/-90 kHz: %d of %d right\n", ok, n);
    CHECK(ok * 100 >= n * 90, "width %d of %d", ok, n);
    // lock from the wrong width
    for (double sr : {7.56e6, 5.67e6}) {
        const R r = run(sr, 25, 20e3, 3.0, true, 77);
        printf("wrong starting width, symbol rate %.2f: found %.0f MHz, state %d\n", sr / 1e6, r.width, r.state);
        CHECK(r.width == (sr < 7e6 ? 6 : 8) && r.state >= 1, "lock %.2f", sr / 1e6);
    }
    // noise alone: nothing measured
    {
        DtmbReceiver rx;
        rx.configure(10e6, 8);
        rx.setDecoderThreads(0);
        rx.setAutoWidth(true);
        std::mt19937 g(5);
        std::normal_distribution<float> d(0, 0.1f);
        std::vector<cf32> buf(40000);
        for (int i = 0; i < 100; i++) { for (auto& v : buf) v = cf32(d(g), d(g)); rx.feed(buf.data(), buf.size()); }
        DtmbTelemetry t;
        rx.telemetry(t, 0);
        printf("noise alone: measured %.0f MHz (0 = nothing)\n", t.autoWidthMhz);
        CHECK(t.autoWidthMhz == 0, "noise width");
    }
    printf("%.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return testSummary("autoparam_dtmb");
}
