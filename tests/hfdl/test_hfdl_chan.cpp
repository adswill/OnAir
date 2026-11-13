// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HFDL shared FFT channeliser against the per-channel path, on a 32 MHz capture: three synthetic bursts (the lowest, a middle and the highest
// dial of the station table) mixed into white noise, the whole table as the channel list, centre 12 MHz. Both receivers get the same samples; the
// number of ACARS frames with a good CRC must be the same (one frame less is allowed at the weakest SNR, where a burst sits at the edge).
// Also: the channeliser alone decodes all three at the strong SNR, so the test does not pass by both decoding nothing.
#include "dect2/hfdl/hfdl_gen.h"
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/dsp/portable_rng.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

static const double kRate = 32e6, kCentre = 12e6;

// the number of good ACARS frames, for the receiver in `mode` (0 per-channel path, 1 channeliser); Es/N0 snrDb of a 0 dB burst
static int run(int mode, double snrDb, uint32_t seed) {
    std::set<double> dials;
    for (const auto& s : hfdlStations()) for (int k : s.khz) dials.insert(k * 1e3);
    const std::vector<double> list(dials.begin(), dials.end());
    const double chs[3] = {list.front(), list[list.size() / 2], list.back()};
    // by default every third dial of the table (and the three with a burst) is the channel list, still a crowded list over the whole span;
    // the long run gives both receivers the whole table
    std::vector<double> use;
    for (size_t i = 0; i < list.size(); i++) if (testLong() || i % 3 == 0 || list[i] == chs[1] || i + 1 == list.size()) use.push_back(list[i]);
    const int m1 = 1;
    const std::vector<uint8_t> pdu = hfdlMpduDown(1, 23, {hfdlUnnumbered(hfdlAcarsHfnpdu("N1UA", "H1", '2', "WIDE CAPTURE TEST", true))});
    HfdlReceiver rx;
    rx.configure(kRate); rx.setCenterHz(kCentre); rx.setChannels(use); rx.setChanneliser(mode);
    std::vector<HfdlWave> waves;
    for (int i = 0; i < 3; i++) {
        HfdlWave w = hfdlMakeWave(m1, pdu, {}, seed + (uint32_t)i);
        w.start = (0.2 + 0.15 * i) * kRate;
        w.carHz = chs[i] + kHfdlCarrierHz - kCentre + 25.0 * (i - 1);
        w.amp = 0.1f; w.phase = 0.3 + seed + i;
        waves.push_back(std::move(w));
    }
    PortableRng rng(seed);   // the same noise for both receivers
    const double sigma = std::sqrt(0.01 / std::pow(10.0, snrDb / 10) * (kRate / kHfdlBaud) / 2);
    const size_t N = 65536;
    std::vector<cf32> buf(N);
    const int64_t total = (int64_t)((0.5 + hfdlBurstSeconds(m1) + 0.6) * kRate);
    for (int64_t pos = 0; pos < total; pos += (int64_t)N) {
        for (auto& v : buf) v = cf32((float)rng.gauss(sigma), (float)rng.gauss(sigma));
        for (const auto& w : waves) hfdlRender(w, buf.data(), pos, N, kRate);
        rx.feed(buf.data(), N);
    }
    HfdlTelemetry t; rx.telemetry(t, ~0ull);
    int n = 0; for (const auto& m : t.messages) n += m.acars && m.acarsCrcOk;
    return n;
}

int main() {
    // strong: every burst decodes on both paths
    {
        const int a = run(0, 12.0, 21), b = run(1, 12.0, 21);
        printf("  12 dB: per-channel path %d frames, channeliser %d frames (of 3 bursts)\n", a, b);
        CHECK(b == a, "12 dB: channeliser %d, per-channel %d", b, a);
        CHECK(b >= 3, "12 dB: the channeliser decoded only %d of 3 bursts", b);
    }
    // weakest: the edge of what m1 1 decodes; one frame of difference is allowed
    for (uint32_t seed : {31u, 32u}) {
        if (seed == 32u && !testLong()) continue;   // one noise seed by default, both in the long run
        const double snr = 3.0;
        const int a = run(0, snr, seed), b = run(1, snr, seed);
        printf("  %.0f dB, noise seed %u: per-channel path %d frames, channeliser %d frames\n", snr, seed, a, b);
        CHECK(std::abs(a - b) <= 1, "%.0f dB seed %u: channeliser %d, per-channel %d", snr, seed, b, a);
    }
    return testSummary("hfdl channeliser");
}
