// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RC model PPM: frames with every channel count, polarity, offset, noise, clock error and timing jitter; noise alone must decode nothing.
#include "x40_gen.h"
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const double fs = 1000000;
    // every channel count, both polarities, with an off-centre carrier and a drifting clock
    for (int n : {2, 4, 6, 8, 9}) for (int inv : {0, 1}) for (double off : {0.0, 12000.0, -15000.0}) for (double ppm : {0.0, 50000.0, -30000.0}) {
        Ppm p; p.chMs.clear(); for (int i = 0; i < n; i++) p.chMs.push_back(1.0 + 0.1 * i); p.invert = inv; p.clockPpm = ppm; p.frameMs = 22.5;
        auto x = x40gen::ppm(p, fs, 1.0); mix(x, off, 0, fs);
        auto t = run(x40::RcPpm, x, fs);
        CHECK(t.blocksOk >= 30, "n=%d inv=%d off=%.0f ppm=%.0f: %llu good", n, inv, off, ppm, (unsigned long long)t.blocksOk);
        CHECK(!t.rows.empty() && t.rows[0].id == std::to_string(n) + " ch", "n=%d inv=%d: row id '%s'", n, inv, t.rows.empty() ? "" : t.rows[0].id.c_str());
    }
    // channel values reach the readout
    {
        Ppm p; p.chMs = {1.0, 1.25, 1.5, 1.75, 2.0}; auto t = run(x40::RcPpm, x40gen::ppm(p, fs, 0.6), fs);
        auto val = [&](const char* k) { for (auto& f : t.facts) if (f.first == k) return atof(f.second.c_str()); return -1.0; };
        CHECK(std::fabs(val("channel 5") - 2.0) < 0.02, "channel 5 value %.3f", val("channel 5"));
        CHECK(std::fabs(val("channel 1") - 1.0) < 0.02, "channel 1 value %.3f", val("channel 1"));
        CHECK(std::fabs(val("channel 3") - 1.5) < 0.02, "channel 3 value %.3f", val("channel 3"));
    }
    // jitter and noise: frames still decode down to a modest SNR; count how many frames survive per SNR (carrier to noise in 40 kHz)
    for (double snr : {30.0, 20.0, 14.0, 10.0}) {
        Ppm p; p.chMs = {1.2, 1.4, 1.6, 1.8, 1.5, 1.3}; p.jitterUs = 8;
        auto x = x40gen::ppm(p, fs, 2.0); addNoise(x, std::pow(10.0, -snr / 10) * fs / 40000.0, 5); mix(x, 8000, 200, fs);
        auto t = run(x40::RcPpm, x, fs);
        printf("snr %.0f dB: %llu good, %llu bad of ~100 frames\n", snr, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        if (snr >= 20) CHECK(t.blocksOk >= 70, "snr %.0f: %llu good", snr, (unsigned long long)t.blocksOk);
        if (snr >= 14) CHECK(t.blocksOk >= 30, "snr %.0f: %llu good", snr, (unsigned long long)t.blocksOk);
    }
    // false decodes: noise only, a steady carrier, and a carrier with slow AM must give nothing
    {
        std::vector<cf32> z(2000000); addNoise(z, 1.0, 9);
        auto t = run(x40::RcPpm, z, fs); CHECK(t.blocksOk == 0 && t.rows.empty(), "noise only decoded %llu frames", (unsigned long long)t.blocksOk);
        std::vector<cf32> c(1000000, cf32(1, 0)); addNoise(c, 0.01, 9);
        t = run(x40::RcPpm, c, fs); CHECK(t.blocksOk == 0, "plain carrier decoded %llu frames", (unsigned long long)t.blocksOk);
        for (size_t i = 0; i < c.size(); i++) c[i] *= (float)(1 + 0.5 * std::sin(2 * kPi * 7.0 * i / fs));
        t = run(x40::RcPpm, c, fs); CHECK(t.blocksOk == 0, "slow AM decoded %llu frames", (unsigned long long)t.blocksOk);
    }
    printf(fails ? "x40 rcppm: %d FAILED\n" : "x40 rcppm: ok\n", fails);
    return fails ? 1 : 0;
}
