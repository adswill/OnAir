// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// mioty (TS-UNB uplink, ETSI TS 103 357) receiver against its own generator: every uplink TSMA pattern, short and extended frames, frequency offsets inside the channel,
// noise sweep, two frames overlapping in time, missing bursts, noise only. Simulated signals only: no real mioty recording has been tried.
#include "dect2/xlink/mioty.h"
#include "dect2/xlink/xlink_rx.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;
using namespace dect2::xlink::mioty;

static std::vector<uint8_t> mpdu(int n, unsigned seed) {
    std::mt19937 g(seed);
    std::vector<uint8_t> m((size_t)n);
    for (auto& b : m) b = (uint8_t)g();
    m[0] = 0x00;                              // MAC header: short address, data only
    m[1] = 0x12; m[2] = 0x34;                 // short address
    return m;
}

static void noise(std::vector<cf32>& x, double sigma, unsigned seed) {
    std::mt19937 g(seed);
    std::normal_distribution<float> n(0, (float)sigma);
    for (auto& v : x) v += cf32(n(g), n(g));
}

struct Got { std::vector<Frame> f; uint64_t pilots = 0; };
static Got run(std::vector<cf32>& x) {
    Receiver rx;
    Got g;
    rx.setCallback([&](const Frame& f) { g.f.push_back(f); });
    for (size_t i = 0; i < x.size(); i += 4000) rx.feed(x.data() + i, std::min<size_t>(4000, x.size() - i));
    rx.flush();
    g.pilots = rx.pilots();
    return g;
}

int main() {
    // coding check: CRC-8 of the 16 bits 0x0000 and the PN9 start are fixed by the spec's parameters
    if (crc8(std::vector<uint8_t>(16, 0)) == 0) { printf("FAIL crc\n"); return 1; }
    const int64_t T = (int64_t)kSps;
    // all 17 patterns, short frames, offsets across the channel
    for (int p = 0; p < 17; p++) {
        const auto m = mpdu(14 + p % 5, (unsigned)p);
        const double f0 = -12 * kCarrierStep + (p % 3 - 1) * 9000.0 + 37.0 * p;
        auto x = txFrame(m, p, kRate, f0, 30 * T, (size_t)(30 * T + 15000 * T));
        noise(x, 0.1, (unsigned)p);
        Got g = run(x);
        if (g.f.size() != 1 || g.f[0].mpdu != m || g.f[0].pattern != p) { printf("FAIL pattern %s: %zu frames, %llu pilots\n", patternName(p), g.f.size(), (unsigned long long)g.pilots); return 1; }
        if (std::fabs(g.f[0].freqHz - f0) > 40) { printf("FAIL freq %s: %.0f want %.0f\n", patternName(p), g.f[0].freqHz, f0); return 1; }
    }
    printf("17 patterns ok\n");
    // extended frames (PSI 21, 30): extension bursts on 25 carriers from the LFSR
    for (int len : {21, 30}) {
        const auto m = mpdu(len, 100u + (unsigned)len);
        auto x = txFrame(m, 2, kRate, -12 * kCarrierStep, 40 * T, (size_t)(40 * T + 30000 * T));
        noise(x, 0.1, 9);
        Got g = run(x);
        if (g.f.size() != 1 || g.f[0].mpdu != m) { printf("FAIL extended %d: %zu frames\n", len, g.f.size()); return 1; }
        printf("extended psi %d ok: %s\n", len, describeMpdu(g.f[0].mpdu).c_str());
    }
    // two frames overlapping in time on different patterns and offsets
    {
        auto a = mpdu(16, 1), b = mpdu(12, 2);
        std::vector<cf32> x((size_t)(16000 * T), cf32(0, 0));
        addFrame(x, a, 0, kRate, -12 * kCarrierStep - 11000, 50 * T);
        addFrame(x, b, 9, kRate, -12 * kCarrierStep + 14000, 1500 * T);
        noise(x, 0.15, 3);
        Got g = run(x);
        int ok = 0;
        for (auto& f : g.f) if (f.mpdu == a || f.mpdu == b) ok++;
        if (ok != 2 || g.f.size() != 2) { printf("FAIL overlap: %zu frames, %d right\n", g.f.size(), ok); return 1; }
    }
    // noise sweep (per-sample sigma; the burst's symbol energy is 48 samples)
    for (double sigma : {1.0, 2.0, 3.0, 4.0}) {
        int ok = 0;
        for (int t = 0; t < 4; t++) {
            const auto m = mpdu(15, 500u + (unsigned)t);
            auto x = txFrame(m, t % 17, kRate, -12 * kCarrierStep + 3000.0 * (t - 3), 30 * T, (size_t)(30 * T + 15000 * T));
            noise(x, sigma, 700u + (unsigned)t);
            Got g = run(x);
            for (auto& f : g.f) if (f.mpdu == m) { ok++; break; }
        }
        const double esn0 = 10 * std::log10(kSps / (2 * sigma * sigma));
        printf("sigma %.1f (Es/N0 %.1f dB): %d/4\n", sigma, esn0, ok);
        if (sigma <= 2.0 && ok < 4) { printf("FAIL noise sweep\n"); return 1; }
    }
    // noise only
    {
        std::vector<cf32> x((size_t)(20000 * T), cf32(0, 0));
        noise(x, 1.0, 42);
        Got g = run(x);
        if (!g.f.empty()) { printf("FAIL noise gave frames\n"); return 1; }
        printf("noise only: %llu pilot detections, no frames\n", (unsigned long long)g.pilots);
    }
    // through XlinkReceiver from a 500 kHz capture, the channel 20 kHz off the middle
    {
        const auto m = mpdu(18, 77);
        const double fs = 500e3;
        auto x = txFrame(m, 5, fs, -12 * kCarrierStep + 20000, (int64_t)(0.2 * fs), (size_t)(7.0 * fs));
        noise(x, 0.3, 5);
        XlinkReceiver rx;
        rx.setProtocols(xlink::Mioty);
        rx.configure(fs);
        for (size_t i = 0; i < x.size(); i += 8192) rx.feed(x.data() + i, std::min<size_t>(8192, x.size() - i));
        rx.flush();
        XlinkTelemetry t;
        rx.telemetry(t, ~0ull);
        if (t.frames.size() != 1 || t.frames[0].proto != xlink::Mioty || t.frames[0].id != "short 1234") { printf("FAIL receiver path: %zu frames\n", t.frames.size()); return 1; }
        if (std::fabs(t.frames[0].freqHz - 20000) > 60) { printf("FAIL receiver freq %.0f\n", t.frames[0].freqHz); return 1; }
        printf("receiver path: %s | %s\n", t.frames[0].type.c_str(), t.frames[0].detail.c_str());
    }
    printf("mioty ok\n");
    return 0;
}
