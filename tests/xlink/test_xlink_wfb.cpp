// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// wfb-ng and OpenHD frames over the air: built with the real header layouts, through the 802.11n transmitter, into XlinkReceiver at 20 and 40 Msps with an offset channel.
// Simulated signals only; no real capture of either link is available.
#include "dect2/xlink/ht_phy.h"
#include "dect2/xlink/xlink_rx.h"
#include "dect2/wifi/wifi_proto.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace dect2;

static std::vector<uint8_t> mac(const uint8_t a2[6], const uint8_t a3[6]) {
    std::vector<uint8_t> f = {0x08, 0x01, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    f.insert(f.end(), a2, a2 + 6);
    f.insert(f.end(), a3, a3 + 6);
    f.push_back(0); f.push_back(0);
    return f;
}

static std::vector<uint8_t> fcs(std::vector<uint8_t> f) {
    const uint32_t c = wifi::crc32(f.data(), f.size());
    for (int i = 0; i < 4; i++) f.push_back((uint8_t)(c >> (8 * i)));
    return f;
}

static std::vector<uint8_t> wfbData(uint32_t cid, uint64_t block, int frag, int payload) {
    uint8_t a[6] = {0x57, 0x42, (uint8_t)(cid >> 24), (uint8_t)(cid >> 16), (uint8_t)(cid >> 8), (uint8_t)cid};
    auto f = mac(a, a);
    f.push_back(1);
    const uint64_t nonce = (block << 8) | (uint64_t)frag;
    for (int i = 7; i >= 0; i--) f.push_back((uint8_t)(nonce >> (8 * i)));
    for (int i = 0; i < payload; i++) f.push_back((uint8_t)(i * 7 + 3));
    return fcs(f);
}

static std::vector<uint8_t> wfbSession(uint32_t cid) {
    uint8_t a[6] = {0x57, 0x42, (uint8_t)(cid >> 24), (uint8_t)(cid >> 16), (uint8_t)(cid >> 8), (uint8_t)cid};
    auto f = mac(a, a);
    f.push_back(2);
    for (int i = 0; i < 24 + 64 + 16; i++) f.push_back((uint8_t)i);
    return fcs(f);
}

static std::vector<uint8_t> openhd(int peer, int port, uint64_t nonce, int payload) {
    uint8_t a2[6], a3[6];
    a2[0] = a3[0] = (uint8_t)peer;
    a2[5] = a3[5] = (uint8_t)port;
    for (int i = 0; i < 4; i++) { a2[1 + i] = (uint8_t)(nonce >> (8 * i)); a3[1 + i] = (uint8_t)(nonce >> (32 + 8 * i)); }
    auto f = mac(a2, a3);
    for (int i = 0; i < payload; i++) f.push_back((uint8_t)(i + 1));
    return fcs(f);
}

static void push(std::vector<cf32>& a, const std::vector<cf32>& b, int gap) {
    a.insert(a.end(), b.begin(), b.end());
    a.resize(a.size() + (size_t)gap, cf32(0, 0));
}

int main() {
    using namespace xlink::ht;
    std::vector<cf32> sig(3000, cf32(0, 0));
    const uint32_t cidA = 0x123456u << 8 | 1, cidB = 0xABCDEFu << 8 | 16;
    push(sig, htPpdu(wfbSession(cidA), 1), 1500);
    // data blocks: FEC (8, 12) fragments, dropping fragment 5 of block 2 and all of block 3 of link A
    for (uint64_t b = 1; b <= 4; b++) {
        if (b == 3) continue;
        for (int fr = 0; fr < 12; fr++) {
            if (b == 2 && fr == 5) continue;
            push(sig, htPpdu(wfbData(cidA, b, fr, 500), 2), 700);
        }
    }
    push(sig, htPpdu(wfbData(cidB, 7, 0, 800), 3, true), 900);
    push(sig, htPpdu(openhd(1, 13, 0x1122334455667788ULL, 1200), 3), 900);
    push(sig, htPpdu(openhd(2, 14, 42, 300), 1), 900);
    // an ordinary Wi-Fi data frame broadcast from an access point (FC from-DS): not ours
    { std::vector<uint8_t> f = {0x08, 0x02, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 2, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0, 1, 0, 0}; f.resize(100, 0x5A); push(sig, htPpdu(fcs(f), 3), 1000); }
    sig.resize(sig.size() + 5000, cf32(0, 0));

    const int expectedWfb = 1 + 12 + 11 + 12 + 1;   // session + block 1 + block 2 (one lost) + block 4 + link B

    for (int run = 0; run < 3; run++) {
        const double fs = run == 2 ? 40e6 : 20e6;
        const double offset = run == 0 ? 0 : 3.1e6 * (run == 1 ? 1 : -1);
        const double cfo = 45e3;
        std::mt19937 g(100 + run);
        std::normal_distribution<float> n(0, 0.003f);
        std::vector<cf32> y;
        if (fs > 20e6) {
            y.resize(sig.size() * 2 + 100, cf32(0, 0));
            // up by two with a linear-phase low pass of a few taps: enough for a test of the resampler path, the signal occupies +-8.75 MHz
            const float h[9] = {-0.03f, -0.0f, 0.28f, 0.5f, 0.28f, -0.0f, -0.03f, 0.f, 0.f};
            std::vector<cf32> up(sig.size() * 2, cf32(0, 0));
            for (size_t i = 0; i < sig.size(); i++) up[2 * i] = sig[i] * 2.f;
            for (size_t i = 8; i < up.size(); i++) { cf32 a(0, 0); for (int t = 0; t < 7; t++) a += up[i - (size_t)t] * h[t]; y[i] = a; }
        } else y = sig;
        double ph = 0.3;
        const double d = 2 * 3.141592653589793 * (cfo + offset) / fs;
        for (auto& v : y) { v *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += d; v += cf32(n(g), n(g)); }

        XlinkReceiver rx;
        rx.setLogCallback([](const std::string&) {});
        rx.setProtocols(1);
        rx.configure(fs);
        rx.setSignalOffset(offset);
        for (size_t i = 0; i < y.size(); i += 4096) rx.feed(y.data() + i, std::min<size_t>(4096, y.size() - i));
        XlinkTelemetry t;
        if (!rx.telemetry(t, ~0ull)) { printf("no telemetry\n"); return 1; }
        int nw = 0, no = 0;
        for (const auto& f : t.frames) { if (f.proto == xlink::WfbNg) nw++; else if (f.proto == xlink::OpenHd) no++; }
        printf("run %d: wfb %d openhd %d links %zu ok %llu bad %llu\n", run, nw, no, t.links.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        if (nw != expectedWfb || no != 2 || t.links.size() != 4) { printf("FAIL counts: frames %zu\n", t.frames.size()); for (auto& f : t.frames) printf("%s | %s | %s | %s\n", xlink::protoName(f.proto), f.id.c_str(), f.type.c_str(), f.detail.c_str()); return 1; }
        // link A: 12 frames of block 1, block 2 missing one, then a jump over block 3
        uint64_t lostA = 999, cntA = 0;
        for (const auto& l : t.links) if (l.id == "link 123456 port 1") { lostA = l.lost; cntA = l.count; }
        if (cntA != 1 + 12 + 11 + 12 || lostA != 1 + 12) { printf("FAIL link A count %llu lost %llu\n", (unsigned long long)cntA, (unsigned long long)lostA); return 1; }
    }
    // below 20 Msps the receiver says so and shows nothing
    {
        XlinkReceiver rx;
        rx.setProtocols(1);
        rx.configure(10e6);
        std::vector<cf32> z(50000, cf32(0, 0));
        rx.feed(z.data(), z.size());
        XlinkTelemetry t;
        rx.telemetry(t, ~0ull);
        if (!t.rateTooLow || !t.frames.empty()) { printf("FAIL low-rate path\n"); return 1; }
    }
    printf("wfb ok\n");
    return 0;
}
