// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP burst PHY: clean loopback of every profile, AWGN at threshold + 3 dB with carrier offset and random start, header at -6 dB, ACK burst,
// preamble false alarms on noise, the radio-rate resampler, two bursts in one stream.
#include "kapp_test_util.h"

using namespace dect2::kapp;
using namespace kapptest;

static BurstSpec makeSpec(int profile, int nBlocks, int seq, Rng& rng) {
    BurstSpec s;
    s.callsign = "LY1ABC";
    s.profileId = profile;
    s.seq = seq;
    s.flags = kFlagAckRequest;
    s.payload = randomBytes(rng, (size_t)nBlocks * (size_t)kProfiles[(size_t)profile].payloadBytes);
    return s;
}

static void testTable() {
    CHECK(profileTableConsistent(), "profile table does not match kris::ldpcK");
    for (const Profile& p : kProfiles) {
        CHECK(p.payloadBytes == (dect2::kris::ldpcK(kBlockN, p.rate) - 16) / 8, "profile %d payload bytes", p.id);
        CHECK(p.bitsPerSym == (int)p.mod, "profile %d bits per symbol", p.id);
    }
    CHECK(profileById(9) == nullptr && profileById(-1) == nullptr && profileById(8) != nullptr, "profile ids");
    // spec section 6: blocks per burst length
    CHECK(maxBlocksForMs(kProfiles[0], 1000.0) == 33, "P0 1 s blocks %d", maxBlocksForMs(kProfiles[0], 1000.0));
    CHECK(maxBlocksForMs(kProfiles[2], 100.0) == 5, "P2 100 ms blocks %d", maxBlocksForMs(kProfiles[2], 100.0));
    CHECK(maxBlocksForMs(kProfiles[6], 1000.0) == 199, "P6 1 s blocks %d", maxBlocksForMs(kProfiles[6], 1000.0));
    // callsign code
    CHECK(decodeCallsign(encodeCallsign("LY1ABC")) == "LY1ABC", "callsign round trip");
    CHECK(decodeCallsign(encodeCallsign("LY1ABC/P")) == "LY1ABC", "long call cut to 7 characters, the trailing / (also the padding code) dropped");
    CHECK(decodeCallsign(encodeCallsign("N0CALL")) == "N0CALL", "short call");
    // header bits
    BurstHeader h;
    h.callsign = "DL1XYZ";
    h.profile = 7;
    h.blocks = 321;
    h.seq = 45;
    h.flags = 5;
    const std::vector<uint8_t> hb = headerInfoBits(h);
    BurstHeader g;
    CHECK(hb.size() == 80 && parseHeaderBits(hb.data(), g) && g.callsign == "DL1XYZ" && g.profile == 7 && g.blocks == 321 && g.seq == 45 && g.flags == 5, "header bits round trip");
    std::vector<uint8_t> bad = hb;
    bad[10] ^= 1;
    CHECK(!parseHeaderBits(bad.data(), g), "header CRC must catch one flipped bit");
    CHECK(headerSymbols(h).size() == (size_t)kHeaderSyms, "header symbol count");
}

static void testClean() {
    Rng rng(11);
    for (int p = 0; p <= kMaxProfileId; ++p) {
        const BurstSpec spec = makeSpec(p, 3, 17 + p, rng);
        const std::vector<cf> burst = buildBurst(spec);
        const Profile& P = kProfiles[(size_t)p];
        const size_t expect = (size_t)(burstSymbols(P, 3) - 1) * kSps + rrcShape().size();
        CHECK(burst.size() == expect, "profile %d burst length %zu, expected %zu", p, burst.size(), expect);
        const std::vector<cf> sig = channel(burst, 300.0, 37.0, rng, 1234, 3000);
        BurstReceiver rx;
        feedChunks(rx, sig, 4096);
        const std::vector<RxBurst> got = rx.takeBursts();
        CHECK(got.size() == 1, "profile %d: %zu bursts decoded", p, got.size());
        if (got.size() != 1) continue;
        const RxBurst& b = got[0];
        CHECK(b.callsign == "LY1ABC" && b.profile == p && b.seq == 17 + p && b.nBlocks == 3 && b.flags == kFlagAckRequest, "profile %d header fields", p);
        CHECK(b.blocksOk() == 3 && b.payload == spec.payload, "profile %d payload (%d of 3 blocks ok)", p, b.blocksOk());
        CHECK(std::fabs(b.cfoHz - 37.0f) < 3.0f, "profile %d cfo %.2f Hz", p, (double)b.cfoHz);
        printf("clean P%d: ok, cfo %.2f Hz, snr %.1f dB\n", p, (double)b.cfoHz, (double)b.snrDb);
    }
}

static void testAwgn() {
    Rng rng(22);
    for (int p = 0; p <= kMaxProfileId; ++p) {
        const Profile& P = kProfiles[(size_t)p];
        const double es = (double)P.needEsN0Db + 3.0;
        const int trials = 10, nb = 3;
        int bad = 0, total = 0;
        double snrSum = 0.0;
        int snrN = 0;
        for (int t = 0; t < trials; ++t) {
            const BurstSpec spec = makeSpec(p, nb, t, rng);
            const std::vector<cf> burst = buildBurst(spec);
            const double cfo = (t & 1) ? 100.0 * rng.uniform() : -100.0 * rng.uniform();
            const size_t lead = 300 + (size_t)rng.below(1500);
            const std::vector<cf> sig = channel(burst, es, cfo, rng, lead, 2000);
            BurstReceiver rx;
            feedChunks(rx, sig, 3001);
            const std::vector<RxBurst> got = rx.takeBursts();
            total += nb;
            if (got.size() != 1 || got[0].nBlocks != nb || got[0].payload.size() != spec.payload.size()) {
                bad += nb;
                continue;
            }
            for (int b = 0; b < nb; ++b) {
                const size_t bp = (size_t)P.payloadBytes;
                const bool same = std::equal(spec.payload.begin() + (long)(b * bp), spec.payload.begin() + (long)((b + 1) * bp), got[0].payload.begin() + (long)(b * bp));
                if (!got[0].blockOk[(size_t)b] || !same) bad++;
            }
            snrSum += (double)got[0].snrDb;
            snrN++;
        }
        const double bler = (double)bad / (double)total;
        printf("awgn P%d at %.1f dB: block error rate %.3f (%d of %d)", p, es, bler, bad, total);
        if (snrN) printf(", mean reported Es/N0 %.2f dB", snrSum / snrN);
        printf("\n");
        CHECK(bler < 0.05, "profile %d block error rate %.3f at threshold + 3 dB", p, bler);
        if (snrN) CHECK(std::fabs(snrSum / snrN - es) < 1.5, "profile %d reported Es/N0 %.2f, true %.2f", p, snrSum / snrN, es);
    }
}

static void testHeader() {
    Rng rng(33);
    const int trials = 60;
    int ok = 0, detected = 0;
    for (int t = 0; t < trials; ++t) {
        const BurstSpec spec = makeSpec(0, 1, t & 63, rng);
        const std::vector<cf> burst = buildBurst(spec);
        const double cfo = 200.0 * (rng.uniform() - 0.5);
        const std::vector<cf> sig = channel(burst, -6.0, cfo, rng, 200 + (size_t)rng.below(900), 2000);
        BurstReceiver rx;
        feedChunks(rx, sig, 2048);
        if (rx.stats().detections > 0) detected++;
        const std::vector<RxBurst> got = rx.takeBursts();
        if (!got.empty() && got[0].callsign == "LY1ABC" && got[0].seq == (t & 63) && got[0].profile == 0) ok++;
    }
    printf("header at -6 dB Es/N0: %d of %d decoded (%d detected)\n", ok, trials, detected);
    CHECK(ok >= (trials * 9) / 10, "header decoded %d of %d at -6 dB", ok, trials);
}

static void testAck() {
    Rng rng(44);
    for (int n : {1, 7, 64, 512}) {
        std::vector<uint8_t> ok((size_t)n);
        for (auto& v : ok) v = (uint8_t)(rng.below(2));
        for (float snr : {-10.f, -3.f, 0.f, 17.f, 53.f}) {
            const std::vector<uint8_t> pl = encodeAckPayload(snr, ok);
            float s2 = 0.f;
            std::vector<uint8_t> bm;
            CHECK(pl.size() == 70 && decodeAckPayload(pl.data(), pl.size(), s2, bm), "ack payload decode");
            CHECK(std::fabs(s2 - snr) < 0.01f, "ack snr %.1f -> %.1f", (double)snr, (double)s2);
            CHECK(bm.size() == 512 && std::equal(ok.begin(), ok.end(), bm.begin()), "ack bitmap n=%d", n);
            for (size_t i = (size_t)n; i < 512; ++i) CHECK(bm[i] == 0, "ack bitmap tail bit %zu", i);
        }
    }
    CHECK(encodeAckPayload(80.f, {})[0] >> 2 == 63 && encodeAckPayload(-40.f, {})[0] >> 2 == 0, "ack snr code is clamped");
    // through the air at 0 dB Es/N0 (the ACK is profile 0)
    AckInfo a;
    a.callsign = "LY2XYZ";
    a.seq = 33;
    a.snrDb = 12.f;
    a.blockOk.assign(300, 0);
    for (size_t i = 0; i < a.blockOk.size(); ++i) a.blockOk[i] = (uint8_t)(rng.below(5) != 0);
    const std::vector<cf> burst = buildAck(a);
    CHECK(burst.size() == (size_t)(burstSymbols(kProfiles[0], 1) - 1) * kSps + rrcShape().size(), "ack burst length");
    CHECK(burstSymbols(kProfiles[0], 1) == 3262 + 74, "ack burst symbols %d (spec 3262 without the 74 pilots)", burstSymbols(kProfiles[0], 1));
    const std::vector<cf> sig = channel(burst, 0.0, -60.0, rng, 700, 2000);
    BurstReceiver rx;
    feedChunks(rx, sig, 4096);
    const std::vector<RxBurst> got = rx.takeBursts();
    CHECK(got.size() == 1 && got[0].isAck() && got[0].profile == 0 && got[0].callsign == "LY2XYZ" && got[0].seq == 33, "ack burst header");
    if (got.size() == 1 && got[0].blocksOk() == 1) {
        float s2 = 0.f;
        std::vector<uint8_t> bm;
        CHECK(decodeAckPayload(got[0].payload.data(), got[0].payload.size(), s2, bm) && std::fabs(s2 - 12.f) < 0.01f &&
                  std::equal(a.blockOk.begin(), a.blockOk.end(), bm.begin()),
              "ack content through the air");
    } else {
        CHECK(false, "ack block did not decode at 0 dB");
    }
}

static void testFalseAlarm() {
    Rng rng(55);
    uint64_t det = 0, hdrOk = 0, bursts = 0;
    const int trials = 1000;
    for (int t = 0; t < trials; ++t) {
        BurstReceiver rx;
        const std::vector<cf> n = noiseOnly(12000, rng);
        feedChunks(rx, n, 4000);
        det += rx.stats().detections;
        hdrOk += rx.stats().headerOk;
        bursts += rx.stats().bursts;
    }
    printf("false alarms: %d noise-only trials, %llu preamble detections, %llu headers accepted, %llu bursts\n", trials, (unsigned long long)det, (unsigned long long)hdrOk, (unsigned long long)bursts);
    CHECK(hdrOk == 0 && bursts == 0, "noise produced %llu accepted headers", (unsigned long long)hdrOk);
}

static void testRateAndStream() {
    Rng rng(66);
    // radio at 1 Msps
    {
        const BurstSpec spec = makeSpec(2, 4, 5, rng);
        const std::vector<cf> sig = channel(buildBurst(spec), 15.0, 80.0, rng, 900, 2500);
        const std::vector<cf> radio = resample(sig, kInternalRate, 1.0e6);
        BurstReceiver::Config cfg;
        cfg.sampleRate = 1.0e6;
        BurstReceiver rx(cfg);
        feedChunks(rx, radio, 5000);
        const std::vector<RxBurst> got = rx.takeBursts();
        CHECK(got.size() == 1 && got[0].blocksOk() == 4 && got[0].payload == spec.payload, "decode through the 1 Msps resampler (%zu bursts)", got.size());
        // and the other way: 2 Msps
        const std::vector<cf> radio2 = resample(sig, kInternalRate, 2.0e6);
        cfg.sampleRate = 2.0e6;
        BurstReceiver rx2(cfg);
        feedChunks(rx2, radio2, 7777);
        const std::vector<RxBurst> got2 = rx2.takeBursts();
        CHECK(got2.size() == 1 && got2[0].blocksOk() == 4 && got2[0].payload == spec.payload, "decode through the 2 Msps resampler (%zu bursts)", got2.size());
    }
    // two bursts with a gap, small chunks, a callback
    {
        const BurstSpec s1 = makeSpec(3, 5, 1, rng), s2 = makeSpec(5, 6, 2, rng);
        std::vector<cf> sig = channel(buildBurst(s1), 25.0, -40.0, rng, 500, 4000);
        const std::vector<cf> sig2 = channel(buildBurst(s2), 25.0, 55.0, rng, 100, 2000);
        sig.insert(sig.end(), sig2.begin(), sig2.end());
        BurstReceiver rx;
        int calls = 0;
        rx.setCallback([&](const RxBurst&) { calls++; });
        feedChunks(rx, sig, 1000);
        const std::vector<RxBurst> got = rx.takeBursts();
        CHECK(got.size() == 2 && calls == 2, "two bursts in a stream: %zu", got.size());
        if (got.size() == 2) {
            CHECK(got[0].seq == 1 && got[0].payload == s1.payload && got[1].seq == 2 && got[1].payload == s2.payload, "two bursts content");
        }
    }
}

int main() {
    testTable();
    testClean();
    testAwgn();
    testHeader();
    testAck();
    testFalseAlarm();
    testRateAndStream();
    if (fails) {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    printf("kapp phy ok\n");
    return 0;
}
