// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HFDL as the issue #66 receiver has it: an RSP1 at 2 Msps, the whole built-in channel list (no channel set by hand), 8.942 MHz
// (Shannon) 401 kHz below a 9.343 MHz centre; also the dial on the LO itself (centre 8.942 MHz, the carrier 1440 Hz above the DC spur)
// and the 8.9 MHz band preset. Each with a tuning error, noise, a strong DC spur and a strong AM carrier 15 kHz away (the Shannon VOLMET
// on 8.957 MHz is in the reporter's recording). The 600 bit/s ACARS burst must decode on 8.942 MHz and nowhere else.
#include "dect2/hfdl/hfdl_gen.h"
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/dsp/portable_rng.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 2e6, kDial = 8.942e6;

static void one(const char* what, double centre, double cfo, double snrDb, uint32_t seed) {
    HfdlReceiver rx;
    rx.setParallel(false);
    rx.configure(kRate);
    rx.setCenterHz(centre);
    const std::vector<uint8_t> pdu = hfdlMpduDown(1, 23, {hfdlUnnumbered(hfdlAcarsHfnpdu("F-GSQA", "H1", '2', "OFFSET TEST 8942", true))});
    HfdlWave w = hfdlMakeWave(1, pdu, {}, seed);
    w.start = 0.25 * kRate; w.carHz = kDial + kHfdlCarrierHz - centre + cfo; w.amp = 0.1f; w.phase = 0.7;
    const double sigma = std::sqrt(0.01 / std::pow(10.0, snrDb / 10) * (kRate / kHfdlBaud) / 2);
    const cf32 spur(0.6f, -0.4f);                               // the DC spur, far above the burst
    const double amHz = 8.957e6 + 0 - centre;                   // VOLMET: an AM carrier 20 dB above the burst, with a 400 Hz tone
    PortableRng rng(seed);
    const size_t N = 65536;
    std::vector<cf32> buf(N);
    const int64_t total = (int64_t)((0.25 + hfdlBurstSeconds(1) + 0.6) * kRate);
    for (int64_t pos = 0; pos < total; pos += (int64_t)N) {
        for (size_t i = 0; i < N; i++) {
            const double t = (double)(pos + (int64_t)i) / kRate;
            const double am = 1.0 * (1 + 0.5 * std::sin(2 * M_PI * 400 * t));
            const double ph = 2 * M_PI * std::fmod(amHz * t, 1.0);
            buf[i] = cf32((float)rng.gauss(sigma), (float)rng.gauss(sigma)) + spur + cf32((float)(am * std::cos(ph)), (float)(am * std::sin(ph)));
        }
        hfdlRender(w, buf.data(), pos, N, kRate);
        rx.feed(buf.data(), N);
    }
    HfdlTelemetry t; rx.telemetry(t, ~0ull);
    int ok = 0, other = 0;
    double gotCfo = 0;
    for (const auto& m : t.messages) {
        if (m.acars && m.acarsCrcOk && std::fabs(m.freqHz - kDial) < 1) { ok++; gotCfo = m.cfoHz; }
        else if (std::fabs(m.freqHz - kDial) >= 1) other++;
    }
    int inBand = 0;
    for (const auto& c : t.channels) inBand += c.inBand && std::fabs(c.freqHz - kDial) < 1;
    printf("  %-44s 8.942 in band %d, ACARS %d (cfo %+.1f Hz), on other channels %d, bad blocks %llu\n", what, inBand, ok, gotCfo, other, (unsigned long long)t.blocksBad);
    CHECK(inBand == 1, "%s: 8.942 MHz not among the channels", what);
    CHECK(ok == 1, "%s: %d good ACARS frames on 8.942 MHz", what, ok);
    CHECK(ok == 0 || std::fabs(gotCfo - cfo) < 5, "%s: cfo %.1f, sent %.1f", what, gotCfo, cfo);
    CHECK(other == 0, "%s: %d messages on other channels", what, other);
}

int main() {
    one("centre 9.343 MHz, +150 Hz, 8 dB", 9.343e6, 150, 8, 3);
    one("centre 9.343 MHz, -200 Hz, 8 dB", 9.343e6, -200, 8, 4);
    one("dial on the LO (8.942 MHz), +80 Hz, 10 dB", 8.942e6, 80, 10, 5);
    one("8.9 MHz band preset, -120 Hz, 6 dB", 8.9e6, -120, 6, 6);
    return testSummary("hfdl offsets");
}
