// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVL physical layer: frame layout, header, interleaver, clean loopback of every modcod, a link with RTL-SDR impairments, no false frames on noise.
#include "kavl_test_util.h"
#include <cstdlib>
#include <cstring>

using namespace kavl;
using namespace kavltest;

static int fails = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
            fails++;                                                      \
        }                                                                 \
    } while (0)

static void testLayout() {
    CHECK(kRegionSyms == static_cast<int>(regionMap().pilot.size() + regionMap().data.size()));
    CHECK(static_cast<int>(regionMap().pilot.size()) == kPilots);
    CHECK(occupiedHz(kDefaultSymRate) <= 500000.0);
    for (int i = 0; i < kNumModcods; i++) {
        const Modcod* m = modcod(i);
        CHECK(m != nullptr);
        CHECK(m->blocks * kLdpcN == kDataSyms * m->bitsPerSym);
        CHECK(m->payloadBytes * 8 + 32 <= m->kBits);
        std::printf("modcod %d %-15s %d code words, %5d bytes/frame, %.3f Mbit/s at %.0f ksym/s (%.0f kHz)\n", i, m->name, m->blocks, framePayloadBytes(*m),
                    netBitRate(*m, kDefaultSymRate) / 1e6, kDefaultSymRate / 1e3, occupiedHz(kDefaultSymRate) / 1e3);
    }
    CHECK(modcod(kNumModcods) == nullptr);
}

static void testHeader() {
    Header h;
    h.modcod = 8;
    h.fec = 5;
    h.layer = 1;
    h.seq = 0xBEEF;
    h.linkQ = 77;
    auto bits = headerBits(h);
    CHECK(bits.size() == 64u);
    Header g;
    CHECK(parseHeader(bits.data(), g));
    CHECK(g.modcod == 8 && g.fec == 5 && g.layer == 1 && g.seq == 0xBEEF && g.linkQ == 77);
    bits[20] ^= 1;
    CHECK(!parseHeader(bits.data(), g));
    CHECK(headerSymbols(h).size() == static_cast<size_t>(kHeaderCodedBits));
}

static void testInterleaver() {
    Rng r(3);
    for (int b = 1; b <= 5; b++) {
        const int n = b * kLdpcN;
        std::vector<uint8_t> in(static_cast<size_t>(n)), tx(static_cast<size_t>(n));
        for (auto& v : in) v = static_cast<uint8_t>(r.below(2));
        interleaveScramble(in.data(), tx.data(), n);
        std::vector<float> l(static_cast<size_t>(n)), back(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) l[static_cast<size_t>(i)] = tx[static_cast<size_t>(i)] ? -1.f : 1.f;
        deinterleaveDescrambleLlr(l.data(), back.data(), n);
        int bad = 0;
        for (int i = 0; i < n; i++) bad += ((back[static_cast<size_t>(i)] < 0) != (in[static_cast<size_t>(i)] != 0));
        CHECK(bad == 0);
    }
}

static void testClean() {
    for (int m = 0; m < kNumModcods; m++) {
        Channel c;   // no noise, no offsets
        const auto res = runLink(m, 2, c, 10 + static_cast<uint64_t>(m));
        std::printf("clean %-15s frames %d/%d code words %d/%d\n", modcod(m)->name, res.framesFound, res.framesSent, res.blocksOk, res.blocks);
        CHECK(res.framesFound == 2);
        CHECK(res.blocksOk == res.blocks);
    }
}

static void testRtlLink() {
    // 256-QAM 10/15 well above its threshold through an RTL-SDR-like receiver at 2.4 Msps
    const auto c = rtlChannel(26.0);
    const auto res = runLink(3, 4, c, 42);
    std::printf("rtl 256-QAM 10/15 at 26 dB: frames %d/%d code words %d/%d, Es/N0 est %.1f dB, MER %.1f dB, offset %.0f Hz, clock %.1f ppm\n", res.framesFound,
                res.framesSent, res.blocksOk, res.blocks, res.esn0Est, res.merDb, res.cfoHz, res.clockPpm);
    CHECK(res.framesFound == 4);
    CHECK(res.blocksOk == res.blocks);
    CHECK(std::fabs(res.cfoHz - c.cfoHz) < 200.0);
    CHECK(std::fabs(res.clockPpm - c.clockPpm) < 10.0);
    // QPSK at a large offset and a narrower width
    auto c2 = rtlChannel(8.0, 380000.0);
    c2.cfoHz = -61000.0;
    const auto r2 = runLink(0, 3, c2, 43);
    std::printf("rtl QPSK 8/15 at 8 dB, 380 ksym/s, -61 kHz: frames %d/%d code words %d/%d, offset %.0f Hz\n", r2.framesFound, r2.framesSent, r2.blocksOk, r2.blocks,
                r2.cfoHz);
    CHECK(r2.framesFound == 3);
    CHECK(r2.blocksOk == r2.blocks);
}

static void testNoiseOnly() {
    Rng rng(7);
    std::vector<cf> x(400000);
    for (auto& v : x) v = cf(static_cast<float>(rng.gauss()), static_cast<float>(rng.gauss()));
    Receiver rx;
    size_t frames = 0;
    for (size_t at = 0; at < x.size(); at += 50000) frames += rx.push(x.data() + at, 50000).size();
    std::printf("noise only: %zu frames, %d header rejects\n", frames, rx.headerRejects());
    CHECK(frames == 0);
}

int main() {
    testLayout();
    testHeader();
    testInterleaver();
    testClean();
    testRtlLink();
    testNoiseOnly();
    if (fails) {
        std::printf("test_kavl_phy: %d failures\n", fails);
        return 1;
    }
    std::printf("test_kavl_phy: all passed\n");
    return 0;
}
