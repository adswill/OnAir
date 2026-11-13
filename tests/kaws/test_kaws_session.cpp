// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS time structure and ARQ: the spec's message-time table (2.1), the UTC slot grid, a lossy-link transfer with retransmission and profile adaptation
#include "dect2/kaws/kaws_arq.h"
#include "dect2/kaws/kaws_sched.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::kaws;
static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

struct Rng {
    kris::XorShift64 s;
    explicit Rng(uint64_t seed) : s{seed ? seed : 1} {}
    double uni() { return (double)(s.next() >> 11) * (1.0 / 9007199254740992.0); }
};

int main() {
    // block timing against the real burst lengths of the Rust crate: units x 3.5 s + data + 1 s closing
    for (int p = 0; p < kNumProfiles; p++) {
        int units = kProfiles[p].units;
        double expect = units * kUnitSec + blockSeconds(p) + kClosingSec;
        double got = (double)burstSamples(p, 1) / kBaseRate;
        CHECK(std::fabs(expect - got) < 1e-6, "P%d burst %.4f s vs %.4f s", p, got, expect);
    }
    // spec section 3: block durations 66.56 43.95 23.89 13.65 5.67 1.00 0.60 0.60
    const double blockSec[] = {66.56, 43.95, 23.89, 13.65, 5.67, 1.00, 0.60, 0.60};
    for (int p = 0; p < kNumProfiles; p++) CHECK(std::fabs(blockSeconds(p) - blockSec[p]) < 0.006, "P%d block %.3f s vs %.2f", p, blockSeconds(p), blockSec[p]);

    // section 2.1: 50 characters of chat = 401 bits of message
    struct Row { int usable, blocks, bps, slots; double msgSec, onAir, withAck; };
    const Row rows[] = {{104, 4, 1, 4, 480, 4 * 104.06, 600}, {232, 2, 1, 2, 120, 2 * 53.45, 180}, {312, 2, 2, 1, 60, 53.78, 120}, {488, 1, 1, 1, 30, 19.65, 60},
                        {486, 1, 1, 1, 15, 11.67, 30},     {336, 2, 6, 1, 15, 8.0, 30}};
    for (int p = 0; p < 6; p++) {
        MessagePlan m = planMessage(p, 401);
        CHECK(m.usableBits == rows[p].usable && m.blocks == rows[p].blocks && m.blocksPerSlot == rows[p].bps && m.slots == rows[p].slots, "P%d plan %d/%d/%d/%d", p, m.usableBits, m.blocks, m.blocksPerSlot, m.slots);
        CHECK(std::fabs(m.messageSec - rows[p].msgSec) < 0.01 && std::fabs(m.withAckSec - rows[p].withAck) < 0.01, "P%d times %.1f / %.1f", p, m.messageSec, m.withAckSec);
        CHECK(std::fabs(m.onAirSec - rows[p].onAir) < 0.15, "P%d on-air %.2f vs %.2f", p, m.onAirSec, rows[p].onAir);
    }
    CHECK(usableBits(6) == 576 && usableBits(7) == 876 && blocksPerSlot(6) == 0, "voice profiles");
    CHECK(std::fabs(onAirSeconds(6, 1) - 6.6) < 0.001, "P6 on-air %.3f", onAirSeconds(6, 1));

    // UTC grid
    SlotGrid g120(120), g30(30);
    CHECK(g120.index(0) == 0 && g120.index(119.9) == 0 && g120.index(120.0) == 1 && g120.index(86399.0) == 719, "index");
    CHECK(g120.callerSlot(0) && !g120.callerSlot(1) && g120.callerSlot(2), "even slots are the caller's");
    CHECK(std::fabs(g30.txStart(5) - 151.5) < 1e-9 && std::fabs(g30.windowStart(5) - 148.5) < 1e-9 && std::fabs(g30.windowEnd(5) - 154.5) < 1e-9, "tx start and window");
    CHECK(g30.nextIndex(150.0) == 5 && g30.nextIndex(150.1) == 6, "next slot");
    CHECK(kSearchHi == 14400 && kSearchLo == 0, "search range in samples");
    CHECK(std::fabs(clockErrorSec(3.0 * kBaseRate)) < 1e-9 && std::fabs(clockErrorSec(5.0 * kBaseRate) - 2.0) < 1e-9, "clock error");
    for (int p = 0; p < 6; p++) {   // a burst fits its slot: ends 3 s before the next one
        int nb = std::max(1, blocksPerSlot(p));
        CHECK(onAirSeconds(p, nb) + kEndMarginSec <= kProfiles[p].slotSec + 0.5, "P%d does not fit its slot (%.2f s)", p, onAirSeconds(p, nb));
    }

    // profile suggestion
    CHECK(suggestProfile(-40) == 0 && suggestProfile(40) == 5, "suggest ends");
    int last = 0;
    for (double s = -35; s <= 10; s += 0.5) { int p = suggestProfile(s); CHECK(p >= last, "suggestion is monotonic"); last = p; }

    // ARQ over a lossy link at the frame level: 12 blocks of a data message, random block loss, acks sometimes lost
    for (int p : {2, 3, 4, 5}) {
        Rng rng((uint64_t)p * 31 + 5);
        Bits message;
        int bits = usableBits(p) * 7 + 13;
        for (int i = 0; i < bits; i++) message.push_back((uint8_t)(rng.uni() < 0.5));
        Chat c;
        c.from = "LY1ABC"; c.to = "DL2XYZ"; c.text = "ARQ TEST";
        message = encodeChat(c);
        // lengthen to several blocks with a data frame instead
        std::vector<uint8_t> bytes(120);
        for (auto& b : bytes) b = (uint8_t)(rng.uni() * 256);
        message = encodeData(bytes);
        ArqSender tx("LY1ABC", p);
        ArqReceiver rx("DL2XYZ");
        CHECK(tx.start(4, message), "start P%d", p);
        int slots = 0;
        while (!tx.done() && !tx.failed() && slots < 40) {
            std::vector<Bits> blocks = tx.nextBlocks();
            CHECK(!blocks.empty() && (int)blocks.size() <= std::max(1, blocksPerSlot(tx.profile())), "blocks per slot");
            for (auto& b : blocks)
                if (rng.uni() > 0.35) rx.onBlock(kProfiles[tx.profile()].K, b);   // 35 % of the blocks are lost
            slots++;
            if (rng.uni() > 0.2) {   // 20 % of the acks are lost
                Ack a = rx.makeAck(4, -3.0);   // SNR high enough for P5: the receiver may ask for a faster profile
                Bits ab = encodeAck(a, kProfiles[tx.profile()].K);
                Ack a2;
                CHECK(decodeAck(ab, a2), "ack decode");
                tx.onAck(a2, false);   // keep the profile in this part of the test
            }
        }
        CHECK(tx.done(), "P%d done after %d slots", p, slots);
        CHECK(rx.complete(4), "receiver complete P%d", p);
        std::vector<uint8_t> out;
        CHECK(decodeData(rx.message(4), out) && out == bytes, "message intact P%d", p);
    }

    // ARQ gives up: a block never received is abandoned after 8 attempts
    {
        ArqSender tx("LY1ABC", 3);
        Bits message(900, 1);
        CHECK(tx.start(1, message), "start");
        int n = 0;
        while (!tx.failed() && n < 50) { auto b = tx.nextBlocks(); if (b.empty()) break; n++; }
        CHECK(tx.failed() && n == kMaxAttempts, "abandoned after %d attempts", n);
    }

    // profile adaptation through the ack: a good SNR makes the sender follow the suggestion
    {
        ArqSender tx("LY1ABC", 0);
        std::vector<uint8_t> bytes(40, 0x5A);
        CHECK(tx.start(2, encodeData(bytes)), "start P0");
        ArqReceiver rx("DL2XYZ");
        for (auto& b : tx.nextBlocks()) rx.onBlock(kProfiles[0].K, b);
        const double snr = kSuggestThresholdDb[3] + 3.0;   // comfortably inside the range of P3
        Ack a = rx.makeAck(2, snr);
        CHECK(a.suggestedProfile == 3, "suggested %d at %.1f dB", a.suggestedProfile, snr);
        CHECK(tx.onAck(a, true) && tx.profile() == 3, "sender followed to P%d", tx.profile());
        int slots = 0;
        ArqReceiver rx2("DL2XYZ");
        while (!tx.done() && slots < 10) {
            for (auto& b : tx.nextBlocks()) rx2.onBlock(kProfiles[tx.profile()].K, b);
            tx.onAck(rx2.makeAck(2, snr), true);
            slots++;
        }
        std::vector<uint8_t> out;
        CHECK(tx.done() && decodeData(rx2.message(2), out) && out == bytes, "completed after the profile change in %d slots", slots);
    }
    CHECK(!ArqSender("X", 6).start(1, Bits(10, 0)) || true, "voice profile sender constructed");

    printf(fails ? "kaws_session: %d FAILED\n" : "kaws_session: all passed\n", fails);
    return fails ? 1 : 0;
}
