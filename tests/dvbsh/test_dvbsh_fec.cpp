// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-SH channel coding: block sizes of all twelve puncturing patterns, the turbo interleaver, noiseless round trips, EFRAME CRCs, the time
// interleaver (terrestrial and long early-late profiles), the OFDM tables and the TPS, and the turbo decoder's bit error rate on an AWGN
// channel against the BPSK capacity limit. ONAIR_TEST_LONG=1 measures more codewords and more points.
#include "dect2/dvbsh/dvbsh.h"
#include "dect2/dvbt/dvbt.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>

using namespace dect2;
using namespace dect2::dvbsh;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void testSizes() {
    const int expect[kNumPatterns] = {61440, 55296, 49152, 43008, 36864, 36864, 30720, 30720, 24576, 24576, 18432, 18432};
    for (int p = 0; p < kNumPatterns; p++) {
        CHECK(ntcb(p) == expect[p], "pattern %d: %d coded bits, expected %d", p, ntcb(p), expect[p]);
        CHECK(nbil(p) == expect[p] / 128 * 126, "pattern %d NBIL", p);
        CHECK(nbil(p) % kIuBits == 0 && codewordIu(p) % kBranches == 0, "pattern %d codeword not whole IU cycles", p);
        CHECK(codewordsPerShFrame(p) * nbil(p) == dataCu(p) * kCuBits, "pattern %d DATA part", p);
    }
    CHECK(ntcb(0, kSigInput) == 5760, "signalling field %d", ntcb(0, kSigInput));
    for (int K : {kTcInput, kSigInput}) {
        const auto& a = turboInterleaver(K);
        std::set<int> s(a.begin(), a.end());
        CHECK((int)a.size() == K && (int)s.size() == K && *s.begin() == 0 && *s.rbegin() == K - 1, "turbo interleaver K=%d not a permutation", K);
    }
}

static void testNoiseless() {
    std::mt19937 rng(5);
    std::vector<uint8_t> info(kTcInput), cw, dec;
    for (int p = 0; p < kNumPatterns; p++) {
        for (auto& b : info) b = rng() & 1;
        encodeCodeword(info.data(), p, cw);
        std::vector<float> llr(cw.size()), coded(ntcb(p));
        for (size_t i = 0; i < cw.size(); i++) llr[i] = cw[i] ? -4.f : 4.f;
        bitDeinterleave(llr.data(), p, coded.data());
        turboDecode(coded.data(), kTcInput, p, 4, dec);
        int err = 0;
        for (int i = 0; i < kTcInput; i++) err += dec[i] != info[i];
        CHECK(err == 0, "pattern %d (%s): %d errors without noise", p, rateName(p), err);
    }
    // the all-zero word encodes to zeros (the tail returns both encoders to the zero state)
    std::fill(info.begin(), info.end(), 0);
    std::vector<uint8_t> coded;
    turboEncode(info.data(), kTcInput, 0, coded);
    int ones = 0;
    for (auto b : coded) ones += b;
    CHECK(ones == 0, "all-zero word gives %d ones", ones);
}

static void testEframe() {
    std::mt19937 rng(9);
    uint8_t pk[188 * 8], back[188 * 8];
    for (int k = 0; k < 8; k++) { pk[k * 188] = 0x47; for (int i = 1; i < 188; i++) pk[k * 188 + i] = (uint8_t)rng(); pk[k * 188 + 1] &= 0x7F; }
    std::vector<uint8_t> ef(kTcInput);
    buildEframe(pk, 17, ef.data());
    CHECK(eframeChecks(ef.data()), "EFRAME checks");
    EframeInfo e = parseEframe(ef.data(), back);
    CHECK(e.headerOk && e.cbcounter == 17 && e.packetsOk == 8 && !memcmp(pk, back, sizeof pk), "EFRAME round trip");
    ef[114 + 1512 * 3 + 40] ^= 1;   // one bit of packet 3
    CHECK(!eframeChecks(ef.data()), "corruption not seen");
    e = parseEframe(ef.data(), back);
    CHECK(e.headerOk && e.packetsOk == 7 && !e.pktOk[3] && (back[3 * 188 + 1] & 0x80), "corrupt packet flagged");
    // the EScrambler sequence starts 0000 0011 1111 0110 (as the DVB energy dispersal)
    uint8_t pr[16];
    prbsBits(16, pr);
    unsigned v = 0;
    for (int i = 0; i < 16; i++) v = (v << 1) | pr[i];
    CHECK(v == 0x03F6, "PRBS %04x", v);
    const uint8_t s[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(crc16Bytes(s, 9) == 0x29B1, "CRC-16 check value %04x", crc16Bytes(s, 9));
}

static void testTimeInterleaver() {
    struct Case { TiConfig c; int lmax; } cases[] = {
        {{1, 48, 1, 0, 0}, 47},              // uniform, all late
        {{4, 48, 1, 0, 0}, 188},
        {{2, 24, 2, 1, 1}, 272 + 46},        // early / late, one SH frame apart
        {{1, 0, 2, 3, 1}, 3 * 272 + 23},     // two non-late slices
        {{1, 16, 3, 10, 0}, 2 * 2720},       // three slices ten SH frames apart (a long interleaver)
    };
    for (auto& cs : cases) {
        std::vector<int> L;
        CHECK(tiDelays(cs.c, kShFrameCu, L), "valid config");
        int lmax = 0;
        for (int v : L) lmax = std::max(lmax, v);
        CHECK(lmax == cs.lmax && L[0] == 0, "Lmax %d expected %d", lmax, cs.lmax);
        TimeInterleaver<uint8_t> tx;
        TimeInterleaver<int8_t> rx;
        tx.configure(L, false);
        rx.configure(L, true);
        const long delay = 48L * lmax;
        const long n = delay + 3000;
        int bad = 0;
        uint8_t b[kIuBits];
        int8_t q[kIuBits], o[kIuBits];
        // output IU i must be input IU i - 48 Lmax
        std::vector<std::vector<uint8_t>> hist;
        std::mt19937 r2(11);
        for (long i = 0; i < n; i++) {
            std::vector<uint8_t> in(kIuBits);
            for (auto& x : in) x = r2() & 1;
            hist.push_back(in);
            tx.process(in.data(), b);
            for (int k = 0; k < kIuBits; k++) q[k] = b[k] ? -10 : 10;
            rx.process(q, o);
            if (i >= delay) for (int k = 0; k < kIuBits; k++) bad += (o[k] < 0) != (hist[(size_t)(i - delay)][k] != 0);
            if (i > delay + 2) hist[(size_t)(i - delay - 2)].clear();
        }
        CHECK(bad == 0, "TI %d/%d/%d/%d/%d: %d bit errors", cs.c.commonMultiplier, cs.c.lateTaps, cs.c.slices, cs.c.sliceDistance, cs.c.nonLateIncrement, bad);
    }
    std::vector<int> L;
    CHECK(!tiDelays({1, 10, 4, 1, 1}, kShFrameCu, L), "38 taps over 3 slices accepted");
    CHECK(!tiDelays({0, 48, 1, 0, 0}, kShFrameCu, L), "common multiplier 0 accepted");
}

static void testOfdmTables() {
    for (int m : {k2K, k8K, k4K}) {
        CHECK(symbolPermutation(m) == dvbt::symbolPermutation(m), "%s permutation differs from DVB-T", modeName(m));
        CHECK(continualPilots(m).size() == (size_t)(m == k2K ? 45 : m == k4K ? 89 : 177), "%s continual pilots", modeName(m));
    }
    CHECK(continualPilots(k1K).size() == 25 && tpsCarriers(k1K).size() == 7, "1K tables");
    for (int m : {k1K, k2K, k4K, k8K}) {
        const auto& H = symbolPermutation(m);
        std::set<int> s(H.begin(), H.end());
        CHECK((int)s.size() == dataCarriers(m) && *s.rbegin() == dataCarriers(m) - 1, "%s permutation", modeName(m));
        for (int l = 0; l < 4; l++) {
            std::vector<uint8_t> roles;
            carrierRoles(m, l, roles);
            int d = 0;
            for (auto r : roles) d += r == 0;
            CHECK(d == dataCarriers(m), "%s symbol %d: %d data carriers", modeName(m), l, d);
        }
    }
    CHECK(std::fabs(sampleRate(8) - 64e6 / 7) < 1 && std::fabs(sampleRate(1.7) - 12.8e6 / 7) < 1, "sample rates");
}

static void testTps() {
    std::mt19937 rng(2);
    for (int trial = 0; trial < 40; trial++) {
        Params p;
        p.mode = trial % 4; p.guard = (trial / 4) % 4; p.mod = trial % 2; p.hier = p.mod ? (trial / 2) % 4 : 0;
        p.patHp = (int)(rng() % 12); p.patLp = (int)(rng() % 12); p.cellId = (int)(rng() & 0xFFFF);
        p.ti = TiConfig{1 + (int)(rng() % 63), 48, 1, 0, 0};
        if (trial % 3 == 1) p.ti = TiConfig{1 + (int)(rng() % 63), 24, 2, (int)(rng() % 256), (int)(rng() % 64)};
        const int sf = trial % 4;
        TpsFrame f[4];
        for (int fi = 0; fi < 4; fi++) {
            auto b = tpsBits(p, fi, sf);
            b[5 + fi] ^= 1; b[60 - fi] ^= 1;   // two errors: the BCH code corrects them
            CHECK(tpsDecode(b.data(), f[fi], 2) && f[fi].length == 37, "TPS frame %d", fi);
        }
        Params q;
        int sfq = -1;
        CHECK(tpsCombine(f, q, sfq), "combine");
        CHECK(q.mode == p.mode && q.guard == p.guard && q.mod == p.mod && q.hier == p.hier && q.patHp == p.patHp && (!p.hier || q.patLp == p.patLp) &&
              q.cellId == p.cellId && q.ti == p.ti && sfq == sf, "TPS fields (trial %d)", trial);
    }
}

// BER of the turbo code (with the bit-wise interleaver and rate adaptation) on BPSK + AWGN at Eb/N0 per information bit
static double measureBer(int pat, double ebn0Db, int cws, int iters, int* frameErr) {
    std::mt19937 rng(77 + pat);
    std::normal_distribution<float> g(0.f, 1.f);
    const double R = (double)kTcInput / nbil(pat);
    const float sigma = (float)std::sqrt(1.0 / (2.0 * R * std::pow(10.0, ebn0Db / 10)));
    std::vector<uint8_t> info(kTcInput), cw, dec;
    std::vector<float> llr, coded(ntcb(pat));
    long err = 0;
    *frameErr = 0;
    for (int c = 0; c < cws; c++) {
        for (auto& b : info) b = rng() & 1;
        encodeCodeword(info.data(), pat, cw);
        llr.resize(cw.size());
        for (size_t i = 0; i < cw.size(); i++) llr[i] = 2.f * ((cw[i] ? -1.f : 1.f) + sigma * g(rng)) / (sigma * sigma);
        bitDeinterleave(llr.data(), pat, coded.data());
        turboDecode(coded.data(), kTcInput, pat, iters, dec, [&](const uint8_t* d) { return !memcmp(d, info.data(), kTcInput); });
        int e = 0;
        for (int i = 0; i < kTcInput; i++) e += dec[i] != info[i];
        err += e;
        *frameErr += e > 0;
    }
    return (double)err / ((double)cws * kTcInput);
}

static void testBer() {
    const bool lng = getenv("ONAIR_TEST_LONG") != nullptr;
    // BPSK-constrained capacity limits (Eb/N0 where a code of that rate could reach zero errors): 1/5 -0.97, 1/3 -0.50, 1/2 +0.19, 2/3 +1.06 dB
    // Measured waterfall (ONAIR_TEST_LONG, 20 codewords a point): no frame errors from about 1.0 dB above the limit for 1/5 and 1/3 and
    // 1.0 dB for 1/2 and 2/3, as published for 3GPP2 turbo codes of this block size with log-MAP decoding
    struct Pt { int pat; double ebn0; double maxBer; double minBer; } pts[] = {
        {4, 0.0, 1, 5e-3},      // 0.5 dB above the rate 1/3 limit: still above the waterfall
        {4, 0.75, 1e-5, 0},     // 1.25 dB above the limit
        {8, 1.45, 1e-5, 0},     // rate 1/2, 1.25 dB above
        {10, 2.3, 1e-5, 0},     // rate 2/3, 1.25 dB above
        {0, 0.3, 1e-5, 0},      // rate 1/5, 1.25 dB above
    };
    const int cws = lng ? 40 : 4;
    std::printf("turbo BER (log-MAP, 8 iterations, %d codewords of 12282 bits per point)\n", cws);
    for (auto& p : pts) {
        int fe = 0;
        const double ber = measureBer(p.pat, p.ebn0, p.minBer > 0 ? 2 : cws, 8, &fe);
        const double unc = 0.5 * std::erfc(std::sqrt(std::pow(10.0, p.ebn0 / 10)));
        std::printf("  rate %s  Eb/N0 %+.2f dB  BER %.2e  frame errors %d  (uncoded BPSK %.2e)\n", rateName(p.pat), p.ebn0, ber, fe, unc);
        CHECK(ber <= p.maxBer && ber >= p.minBer, "rate %s at %.1f dB: BER %.2e outside [%.0e, %.0e]", rateName(p.pat), p.ebn0, ber, p.minBer, p.maxBer);
    }
    if (lng) {
        for (int pat : {0, 4, 8, 10}) {   // the waterfall of four rates
            static const double limit[kNumPatterns] = {-0.97, -0.90, -0.79, -0.65, -0.50, -0.50, -0.23, -0.23, 0.19, 0.19, 1.06, 1.06};
            for (double e = limit[pat]; e <= limit[pat] + 1.51; e += 0.25) {
                int fe = 0;
                const double ber = measureBer(pat, e, 20, 8, &fe);
                std::printf("  sweep rate %s  Eb/N0 %+.2f dB  BER %.2e  FER %d/20\n", rateName(pat), e, ber, fe);
            }
        }
        for (int pat = 0; pat < kNumPatterns; pat++) {
            int fe = 0;
            static const double limit[kNumPatterns] = {-0.97, -0.90, -0.79, -0.65, -0.50, -0.50, -0.23, -0.23, 0.19, 0.19, 1.06, 1.06};
            const double e = limit[pat] + 1.5;   // 1.5 dB above the BPSK capacity limit of the rate
            const double ber = measureBer(pat, e, 10, 8, &fe);
            std::printf("  pattern %2d rate %s  Eb/N0 %+.1f dB  BER %.2e\n", pat, rateName(pat), e, ber);
        }
    }
}

int main() {
    testSizes();
    testNoiseless();
    testEframe();
    testTimeInterleaver();
    testOfdmTables();
    testTps();
    testBer();
    std::printf(fails ? "dvbsh_fec: %d failures\n" : "dvbsh_fec: all passed\n", fails);
    return fails ? 1 : 0;
}
