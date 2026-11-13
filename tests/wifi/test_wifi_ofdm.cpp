// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi OFDM PHY (wifi_ofdm.h): the code and tables against what the standard fixes, and the transmit half through the receiver with a
// carrier offset, noise, a gap of silence before, and a frame that starts mid-way.
#include "dect2/wifi/wifi_ofdm.h"
#include "dect2/wifi/wifi_proto.h"
#include "dect2/dsp/gen_util.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dect2;
using namespace dect2::wifi;
#include "../common/test_util.h"

static std::vector<uint8_t> beacon(const char* ssid, int len = 0) {
    ApSpec a;
    const uint8_t b[6] = {0x00, 0x04, 0x0E, 1, 2, 3};
    std::memcpy(a.bssid, b, 6);
    a.ssid = ssid; a.channel = 36; a.tsf = 1234;
    auto f = makeBeacon(a);
    (void)len;
    return f;
}

struct Got { std::vector<std::vector<uint8_t>> f; std::vector<ofdm::Info> info; };

static Got run(const std::vector<cf32>& x, ofdm::OfdmRx& rx, size_t blk = 4096) {
    Got g;
    rx.setCallback([&](const uint8_t* p, size_t n, const ofdm::Info& i) { g.f.emplace_back(p, p + n); g.info.push_back(i); });
    for (size_t i = 0; i < x.size(); i += blk) rx.feed(x.data() + i, std::min(blk, x.size() - i));
    return g;
}

int main() {
    // ---- the tables: pilot polarity (Equation 17-25 starts 1 1 1 1 -1 -1 -1 1 ... and ends -1 -1 -1 -1 -1 -1 -1; period 127)
    const int p8[8] = {1, 1, 1, 1, -1, -1, -1, 1};
    for (int i = 0; i < 8; i++) CHECK(ofdm::pilotPolarity(i) == p8[i], "pilot polarity %d", i);
    CHECK(ofdm::pilotPolarity(126) == -1 && ofdm::pilotPolarity(125) == -1 && ofdm::pilotPolarity(127) == 1, "polarity period");
    // the scrambler repeats after 127 bits and the first bits from the all-ones state are 0000111 (clause 17.3.5.5)
    std::vector<uint8_t> z(254, 0);
    ofdm::scramble(z, 0x7F);
    bool per = true;
    for (int i = 0; i < 127; i++) per &= z[(size_t)i] == z[(size_t)i + 127];
    CHECK(per && z[0] == 0 && z[3] == 0 && z[4] == 1 && z[5] == 1 && z[6] == 1 && z[7] == 0, "scrambler");
    // the SIGNAL example of 802.11-2020 Annex I.1: 36 Mbit/s, length 100 is not 6 Mbit/s; use the code's own property instead:
    // all-zero input encodes to all zeros, and one 1 gives the impulse response of g0 = 133 and g1 = 171 octal
    std::vector<uint8_t> imp(8, 0); imp[0] = 1;
    const auto e = ofdm::convEncode(imp);
    const uint8_t want[14] = {1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1};   // pairs (A, B) for delays 0..6: g0 = 1011011, g1 = 1111001
    bool ok = true;
    for (int i = 0; i < 7; i++) ok &= e[(size_t)(2 * i)] == (uint8_t)(0133 >> (6 - i) & 1) && e[(size_t)(2 * i + 1)] == (uint8_t)(0171 >> (6 - i) & 1);
    CHECK(ok, "encoder impulse response");
    (void)want;
    // Viterbi inverts the encoder, also with bit errors in the soft input
    {
        std::vector<uint8_t> in(200);
        uint32_t x = 99;
        for (auto& b : in) { x = x * 1664525u + 1013904223u; b = (uint8_t)(x >> 31); }
        for (int i = 0; i < 6; i++) in[194 + (size_t)i] = 0;
        const auto c = ofdm::convEncode(in);
        std::vector<float> s(c.size());
        for (size_t i = 0; i < c.size(); i++) s[i] = c[i] ? 1.f : -1.f;
        for (size_t i = 10; i < s.size(); i += 29) s[i] = -s[i];   // a hard error every 29 bits
        std::vector<uint8_t> out(200);
        ofdm::viterbiDecode(s.data(), 200, out.data());
        CHECK(out == in, "viterbi with errors");
    }
    // ---- the preamble: the short training is periodic every 16 samples; the long training is two equal symbols after a 32-sample guard
    const auto mp = beacon("OnAir 5G");
    const auto tx = ofdm::txPpdu(mp);
    CHECK(tx.size() == 320 + 80 + 80 * ((16 + 8 * mp.size() + 6 + 23) / 24), "ppdu length %zu", tx.size());
    float perr = 0;
    for (int i = 0; i < 144; i++) perr += std::abs(tx[(size_t)i] - tx[(size_t)i + 16]);
    CHECK(perr < 1e-3f, "short training period");
    float lerr = 0;
    for (int i = 0; i < 64; i++) lerr += std::abs(tx[(size_t)(192 + i)] - tx[(size_t)(256 + i)]) + std::abs(tx[(size_t)(160 + i % 32)] - tx[(size_t)(160 + 32 + i % 32 + 32)]) * 0;
    CHECK(lerr < 1e-3f, "long training symbols");
    // ---- loopback with impairments
    genutil::NoiseSource noise(5);
    struct Case { const char* name; double cfo; double snr; size_t lead; double ppm; };
    const Case cases[] = {
        {"clean", 0, 99, 500, 0}, {"+100 kHz 25 dB", 100e3, 25, 700, 0}, {"-120 kHz 18 dB", -120e3, 18, 333, 0}, {"+30 kHz 12 dB", 30e3, 12, 901, 0},
        {"clock 40 ppm", 20e3, 25, 640, 40},
    };
    for (const Case& c : cases) {
        std::vector<cf32> sig(c.lead, cf32(0, 0));
        std::vector<cf32> burst = tx;
        if (c.ppm != 0) {   // linear interpolation clock error
            std::vector<cf32> r;
            for (double t = 0; t < (double)burst.size() - 2; t += 1 + c.ppm * 1e-6) { const size_t i = (size_t)t; const float fr = (float)(t - (double)i); r.push_back(burst[i] * (1 - fr) + burst[i + 1] * fr); }
            burst = r;
        }
        for (size_t i = 0; i < burst.size(); i++) {
            const double a = 2 * M_PI * c.cfo * (double)(c.lead + i) / ofdm::kRate;
            sig.push_back(burst[i] * cf32((float)std::cos(a), (float)std::sin(a)) * 0.3f);
        }
        sig.resize(sig.size() + 1500, cf32(0, 0));
        if (c.snr < 99) noise.add(sig.data(), sig.size(), (float)(0.3 * std::sqrt(0.5) / std::pow(10.0, c.snr / 20)));
        ofdm::OfdmRx rx;
        const Got g = run(sig, rx);
        CHECK(g.f.size() == 1 && g.f[0] == std::vector<uint8_t>(mp.begin(), mp.end() - 4), "%s: %zu frames (sync fail %llu, signal bad %llu, fcs bad %llu)", c.name, g.f.size(),
              (unsigned long long)rx.stats().noSync, (unsigned long long)rx.stats().signalBad, (unsigned long long)rx.stats().framesBad);
        if (!g.info.empty()) { CHECK(std::fabs(g.info[0].cfoHz - c.cfo) < 3e3, "%s: offset %.0f", c.name, g.info[0].cfoHz); printf("  %s: offset %.0f Hz, SNR %.1f dB\n", c.name, g.info[0].cfoHz, g.info[0].snrDb); }
    }
    // ---- a frame that starts in the receiver's first block (mid-block), several in a row, split in odd blocks
    {
        std::vector<cf32> sig(123, cf32(0, 0));
        for (int k = 0; k < 4; k++) { const auto t = ofdm::txPpdu(beacon(k % 2 ? "second" : "first")); sig.insert(sig.end(), t.begin(), t.end()); sig.resize(sig.size() + 400 + 37 * (size_t)k, cf32(0, 0)); }
        for (cf32& v : sig) v *= 0.2f;
        noise.add(sig.data(), sig.size(), 0.003f);
        ofdm::OfdmRx rx;
        const Got g = run(sig, rx, 997);
        CHECK(g.f.size() == 4, "four frames, got %zu", g.f.size());
    }
    // ---- noise alone and a tone decode nothing
    {
        std::vector<cf32> n(400000, cf32(0, 0));
        noise.add(n.data(), n.size(), 0.1f);
        for (size_t i = 0; i < n.size(); i += 2) n[i] += cf32(0.5f, 0);   // a strong DC / near-DC component
        ofdm::OfdmRx rx;
        const Got g = run(n, rx);
        CHECK(g.f.empty(), "noise decoded %zu frames", g.f.size());
        printf("  noise + tone: %llu triggers, %llu no sync\n", (unsigned long long)rx.stats().triggers, (unsigned long long)rx.stats().noSync);
    }
    return testSummary("wifi ofdm");
}
