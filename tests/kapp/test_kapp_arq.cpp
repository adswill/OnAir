// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP selective repeat and profile stepping, plus one run through the real PHY.
#include "kapp_test_util.h"

using namespace dect2::kapp;
using namespace kapptest;

static void testIntervals() {
    IntervalSet s;
    addInterval(s, 10, 20);
    addInterval(s, 30, 40);
    addInterval(s, 20, 30);
    CHECK(s.size() == 1 && s.begin()->first == 10 && s.begin()->second == 40, "intervals merge");
    subtractInterval(s, 15, 25);
    CHECK(s.size() == 2 && s.at(10) == 15 && s.at(25) == 40, "interval split");
    subtractInterval(s, 0, 100);
    CHECK(s.empty(), "interval clear");
    uint8_t buf[70];
    const uint8_t d[5] = {1, 2, 3, 4, 5};
    CHECK(packArqBlock(0x7ABCDE, true, d, 5, buf, sizeof(buf)), "pack");
    uint32_t off = 0;
    bool last = false;
    const uint8_t* dp = nullptr;
    size_t len = 0;
    CHECK(unpackArqBlock(buf, sizeof(buf), off, last, dp, len) && off == 0x7ABCDE && last && len == 5 && dp[4] == 5, "unpack");
    CHECK(!packArqBlock(1u << 23, false, d, 5, buf, sizeof(buf)) && !packArqBlock(0, false, d, 67, buf, 70), "pack limits");
}

static void testTransfer() {
    Rng rng(7);
    const std::vector<uint8_t> msg = randomBytes(rng, 100000);
    const double losses[] = {0.0, 0.05, 0.20, 0.50};
    for (double loss : losses) {
        const SimResult r = simulateTransfer(msg, 2, loss, 1000 + (uint64_t)(loss * 100), 64);
        printf("selective repeat 100 kB at P2, block loss %.0f%%: %d bursts, %d blocks, efficiency %.3f (limit %.3f)\n", loss * 100.0, r.bursts, r.blocks, r.efficiency,
               0.9 * (1.0 - loss));
        CHECK(r.delivered && r.matches, "message not delivered at loss %.2f", loss);
        if (loss == 0.05 || loss == 0.20) CHECK(r.efficiency >= 0.9 * (1.0 - loss), "efficiency %.3f at loss %.2f", r.efficiency, loss);
        if (loss == 0.0) CHECK(r.efficiency > 0.95, "efficiency %.3f without loss", r.efficiency);
    }
    // every profile, small message, lost ACKs too
    for (int p = 0; p <= kMaxProfileId; ++p) {
        const std::vector<uint8_t> m = randomBytes(rng, 5000 + (size_t)p * 77);
        const SimResult r = simulateTransfer(m, p, 0.3, 50 + (uint64_t)p, 16, 0.2);
        CHECK(r.delivered && r.matches, "profile %d with lost blocks and lost ACKs", p);
    }
    // a tiny message: one block, the last flag
    const std::vector<uint8_t> tiny = {42};
    const SimResult t = simulateTransfer(tiny, 0, 0.0, 3);
    CHECK(t.delivered && t.matches && t.bursts == 1 && t.blocks == 1, "one byte message");
}

static void testResendAtOtherProfile() {
    Rng rng(9);
    const std::vector<uint8_t> msg = randomBytes(rng, 3000);
    ArqSender tx("TXCALL");
    ArqReceiver rx("RXCALL");
    CHECK(tx.setMessage(msg), "set message");
    int prof = 8;
    for (int guard = 0; guard < 500 && !tx.done(); ++guard) {
        const BurstSpec s = tx.nextBurst(prof, 6);
        const size_t bp = (size_t)kProfiles[(size_t)prof].payloadBytes;
        RxBurst b;
        b.profile = prof;
        b.seq = s.seq;
        b.nBlocks = (int)(s.payload.size() / bp);
        b.blockOk.assign((size_t)b.nBlocks, 1);
        b.payload = s.payload;
        if (b.nBlocks > 1) {   // lose the first block of every burst, then change profile: the data must still arrive
            b.blockOk[0] = 0;
            std::fill(b.payload.begin(), b.payload.begin() + (long)bp, (uint8_t)0);
        }
        const AckInfo a = rx.onBurst(b);
        CHECK(tx.onAck(a.seq, a.blockOk), "ack accepted");
        prof = (prof + 3) % 9;   // 8, 2, 5, 8 ...
    }
    CHECK(tx.done() && rx.complete() && rx.message() == msg, "resends at other profiles deliver the message");
    // a stale ACK is ignored
    ArqSender t2("TXCALL");
    t2.setMessage(msg);
    const BurstSpec s2 = t2.nextBurst(2, 4);
    CHECK(!t2.onAck((s2.seq + 5) & 63, std::vector<uint8_t>(4, 1)) && t2.hasOutstanding(), "wrong sequence number is ignored");
}

static void testController() {
    ProfileController c(2);
    CHECK(c.profile() == 2, "start profile");
    // margin 5 dB on P2 (needs 1.5): next needs 4.1 + 1.5 = 5.6 <= 6.5: two ACKs in a row
    c.onAck(6.5f, 10, 0);
    CHECK(c.profile() == 2, "one good ACK does not step up");
    c.onAck(6.5f, 10, 0);
    CHECK(c.profile() == 3, "two good ACKs step up, now %d", c.profile());
    // too little room above the next profile: no step
    ProfileController d(2);
    d.onAck(4.6f, 10, 0);
    d.onAck(4.6f, 10, 0);
    CHECK(d.profile() == 2, "no step up without room (next needs %.1f + 1.5)", (double)kProfiles[3].needEsN0Db);
    // failures
    ProfileController e(5);
    e.onAck(14.f, 10, 3);   // 30 percent
    CHECK(e.profile() == 4, "down one on 30 percent failures, now %d", e.profile());
    e.onAck(14.f, 10, 3);
    CHECK(e.profile() == 2, "down two on the second failing ACK, now %d", e.profile());
    // hold after a step down: 4 bursts without stepping up
    int steps = 0;
    for (int i = 0; i < 4; ++i) {
        const int before = e.profile();
        e.onAck(30.f, 10, 0);
        if (e.profile() != before) steps++;
    }
    CHECK(steps == 0 && e.profile() == 2, "hold after step down, profile %d", e.profile());
    e.onAck(30.f, 10, 0);
    e.onAck(30.f, 10, 0);
    CHECK(e.profile() == 3, "steps up again after the hold, profile %d", e.profile());
    // lost ACK: down two; low margin: down one; clamps
    ProfileController f(4);
    f.onTimeout();
    CHECK(f.profile() == 2, "lost ACK steps down two");
    ProfileController g(3);
    g.onAck(kProfiles[3].needEsN0Db + 0.5f, 10, 0);
    CHECK(g.profile() == 2, "margin below 1 dB steps down one, now %d", g.profile());
    ProfileController h(0);
    h.onTimeout();
    CHECK(h.profile() == 0, "clamped at 0");
    ProfileController k(8);
    for (int i = 0; i < 6; ++i) k.onAck(60.f, 10, 0);
    CHECK(k.profile() == 8, "clamped at 8");
}

// one message through the radio simulation: data bursts and ACK bursts both modulated, noisy and offset
static void testOverTheAir() {
    Rng rng(21);
    const std::vector<uint8_t> msg = randomBytes(rng, 1500);
    ArqSender tx("LY1AAA");
    ArqReceiver rx("LY1BBB");
    tx.setMessage(msg);
    const int prof = 2;
    int ackRx = 0;
    for (int guard = 0; guard < 40 && !tx.done(); ++guard) {
        const BurstSpec s = tx.nextBurst(prof, 4);
        BurstReceiver rxr;
        feedChunks(rxr, channel(buildBurst(s), 9.0, 60.0, rng, 400, 1500), 4096);
        const std::vector<RxBurst> got = rxr.takeBursts();
        if (got.empty()) continue;   // lost: the sender times out
        const AckInfo a = rx.onBurst(got[0]);
        BurstReceiver txr;
        feedChunks(txr, channel(buildAck(a), 9.0, -50.0, rng, 300, 1500), 4096);
        const std::vector<RxBurst> ag = txr.takeBursts();
        if (ag.empty() || !ag[0].isAck() || ag[0].blocksOk() != 1) continue;
        float snr = 0.f;
        std::vector<uint8_t> bm;
        if (!decodeAckPayload(ag[0].payload.data(), ag[0].payload.size(), snr, bm)) continue;
        ackRx++;
        tx.onAck(ag[0].seq, bm);
    }
    printf("over the air: %llu bursts, %d ACKs received\n", (unsigned long long)tx.burstsSent(), ackRx);
    CHECK(tx.done() && rx.complete() && rx.message() == msg, "1500 byte message over the simulated radio link");
}

int main() {
    testIntervals();
    testTransfer();
    testResendAtOtherProfile();
    testController();
    testOverTheAir();
    if (fails) {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    printf("kapp arq ok\n");
    return 0;
}
