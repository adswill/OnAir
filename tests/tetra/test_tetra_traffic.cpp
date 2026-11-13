// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA speech traffic channel (EN 300 395-2 clause 5.5): the channel decoder returns the two 137 bit speech frames that the transmit side of the
// clause (encodeTchS, written from the same clause) coded; class sizes and order against table 4; the per-call encryption gate; encrypted and
// unknown-state calls produce no speech frames. No speech codec is involved: a fake plug-in just records the frames it is given.
#include "dect2/tetra/tetra_phy.h"
#include "dect2/tetra/tetra_traffic.h"
#include "dect2/tetra/tetra_voice.h"
#include "dect2/tetra/tetra_voice_codec.h"
#include <algorithm>
#include <cstdio>
#include <random>
using namespace dect2::tetra;
#include "../common/test_util.h"

static std::vector<uint8_t> randomFrame(std::mt19937& g) {
    std::vector<uint8_t> f(kTchSFrameBits);
    for (auto& b : f) b = (uint8_t)(g() & 1);
    return f;
}

// Table 4 of EN 300 395-2, the sensitivity class of each speech frame bit number (1-based), written out from the printed ranges
static int classOf(int b) {
    auto in = [&](int lo, int hi) { return b >= lo && b <= hi; };
    if (in(1, 4) || in(9, 12) || in(18, 21) || in(27, 32) || in(51, 53) || in(78, 80) || in(105, 107) || in(132, 134)) return 2;
    if (in(5, 8) || in(13, 17) || in(22, 26) || in(33, 34) || in(57, 60) || in(84, 87) || in(111, 114) || in(44, 46) || in(71, 73) || in(98, 100) || in(125, 127)
        || b == 49 || b == 76 || b == 103 || b == 130 || b == 50 || b == 77 || b == 104 || b == 131 || in(54, 55) || in(81, 82) || in(108, 109) || in(135, 136)) return 1;
    return 0;
}

// A speech burst as the receiver sees it: type-4 bits scrambled with the cell code, as soft values
static void makeBurst(const uint8_t* A, const uint8_t* B, uint32_t scr, std::vector<float>& l) {
    uint8_t t4[kTchSBurstBits];
    encodeTchS(A, B, t4);
    scramble(t4, kTchSBurstBits, scr);
    l.resize(kTchSBurstBits);
    for (int i = 0; i < kTchSBurstBits; i++) l[(size_t)i] = t4[i] ? 1.f : -1.f;
}

struct Recorder final : dect2::codec::SpeechCodec {
    std::vector<std::vector<uint8_t>>* frames;
    explicit Recorder(std::vector<std::vector<uint8_t>>* f) : frames(f) {}
    int sampleRate() const override { return 8000; }
    std::vector<int16_t> decode(const std::vector<uint8_t>& c) override { frames->push_back(c); return std::vector<int16_t>(240, 0); }
};

int main() {
    std::mt19937 g(77);
    {   // table 5 order against the classes of table 4: 51 class 0 pairs, then 56 class 1, then 30 class 2, every bit once
        const uint8_t* o = tchSBitOrder();
        int seen[138] = {};
        bool okOrder = true;
        for (int p = 0; p < 137; p++) {
            seen[o[p]]++;
            const int want = p < 51 ? 0 : p < 107 ? 1 : 2;
            if (classOf(o[p]) != want) okOrder = false;
        }
        bool once = true;
        for (int b = 1; b <= 137; b++) if (seen[b] != 1) once = false;
        CHECK(once, "table 5 lists each speech frame bit once");
        CHECK(okOrder, "table 5 order: class 0, class 1, class 2 as in table 4");
        int n[3] = {};
        for (int b = 1; b <= 137; b++) n[classOf(b)]++;
        CHECK(n[0] == 51 && n[1] == 56 && n[2] == 30, "class sizes 51/56/30 (%d/%d/%d)", n[0], n[1], n[2]);
    }
    {   // round trip of random frame pairs through the standard's coding
        bool ok = true;
        for (int t = 0; t < 200; t++) {
            const auto A = randomFrame(g), B = randomFrame(g);
            uint8_t t4[kTchSBurstBits];
            encodeTchS(A.data(), B.data(), t4);
            bool crc = false;
            const std::vector<uint8_t> r = decodeTchS(t4, kTchSBurstBits, &crc);
            if (!crc || r.size() != 2 * kTchSFrameBits || !std::equal(A.begin(), A.end(), r.begin()) || !std::equal(B.begin(), B.end(), r.begin() + kTchSFrameBits)) ok = false;
        }
        CHECK(ok, "200 random frame pairs survive encode / decode");
        CHECK(decodeTchS(nullptr, 0).empty(), "null input: nothing");
        uint8_t z[10] = {};
        CHECK(decodeTchS(z, 10).empty(), "short input: nothing");
    }
    {   // every single bit error inside the protected part (type-3 bits 103..432) is corrected; a corrupted class 2 bit pair beyond the code fails the CRC
        const auto A = randomFrame(g), B = randomFrame(g);
        uint8_t t4[kTchSBurstBits];
        encodeTchS(A.data(), B.data(), t4);
        int bad = 0;
        for (int pos = 0; pos < kTchSBurstBits; pos++) {
            const int i = pos / 24, j = pos % 24, t3i = j * 18 + i;
            if (t3i < 102) continue;   // class 0 is not protected
            uint8_t x[kTchSBurstBits];
            std::copy(t4, t4 + kTchSBurstBits, x);
            x[pos] ^= 1;
            const std::vector<uint8_t> r = decodeTchS(x, kTchSBurstBits);
            if (r.size() != 2 * kTchSFrameBits || !std::equal(A.begin(), A.end(), r.begin()) || !std::equal(B.begin(), B.end(), r.begin() + kTchSFrameBits)) bad++;
        }
        CHECK(bad == 0, "single bit errors in the protected bits are corrected (%d not)", bad);
        // many errors in a row: the CRC (BFI) must refuse the burst
        uint8_t x[kTchSBurstBits];
        std::copy(t4, t4 + kTchSBurstBits, x);
        for (int k = 0; k < 120; k++) x[(size_t)(150 + k)] ^= 1;
        bool crc = true;
        const std::vector<uint8_t> r = decodeTchS(x, kTchSBurstBits, &crc);
        // the 8 bit check lets a hopeless burst through with chance 1/256, so the test only demands that the original is not returned as if it were fine
        CHECK(r.empty() ? !crc : !std::equal(A.begin(), A.end(), r.begin()), "a burst with 120 errors does not come back as the original");
    }
    {   // the call state tracker: only a set-up PDU that says "clear" makes a slot decodable
        CallTracker t;
        CallEvent su; su.kind = 1; su.callId = 100; su.enc = 0; su.slots = 0x4; su.carrier = 1234;   // timeslot 2 (bit 2)
        t.onEvent(su, 10.0);
        CHECK(!t.slotState(2, 10.0).known, "main carrier unknown: the event is ignored");
        t.setMainCarrier(1234);
        t.onEvent(su, 10.0);
        CallEncryption s2 = t.slotState(2, 11.0);
        CHECK(s2.known && s2.encMode == 0 && voiceVerdict(s2) == VoiceVerdict::Passed, "D-SETUP with encryption flag 0: slot 2 clear");
        CHECK(!t.slotState(1, 11.0).known && !t.slotState(3, 11.0).known, "other slots stay unknown");
        CHECK(!t.slotState(2, 10.0 + CallTracker::kTtlSec + 1).known, "the state expires");
        CallEvent cn; cn.kind = 2; cn.callId = 100; cn.enc = -1; cn.slots = 0x4; cn.carrier = 1234;
        t.onEvent(cn, 12.0);
        CHECK(t.slotState(2, 12.5).known && t.slotState(2, 12.5).encMode == 0, "D-CONNECT without the element changes nothing");
        cn.enc = 1;
        t.onEvent(cn, 13.0); t.onEvent(cn, 13.2);   // 2 identical messages
        CHECK(voiceVerdict(t.slotState(2, 13.5)) == VoiceVerdict::EncryptedNotDecoded, "a PDU of the call saying encrypted wins");
        t.onEvent(su, 14.0);
        CHECK(voiceVerdict(t.slotState(2, 14.5)) == VoiceVerdict::Passed, "a clear set-up is accepted at once, even after an encrypted state");
        CallEvent rel; rel.kind = 3; rel.callId = 100;
        t.onEvent(rel, 15.0);
        CHECK(voiceVerdict(t.slotState(2, 15.5)) == VoiceVerdict::StateUnknownNotDecoded, "released call: unknown again");
        CallEvent other = su; other.carrier = 99;
        t.onEvent(other, 16.0);
        CHECK(!t.slotState(2, 16.5).known, "channel allocation on another carrier: ignored");
        CallEvent noSlot = su; noSlot.slots = -1; noSlot.carrier = -1;
        t.onEvent(noSlot, 17.0);
        CHECK(!t.slotState(2, 17.5).known, "no channel allocation: no slot, never clear");
        CallEvent first; first.kind = 2; first.callId = 5; first.enc = 1; first.slots = 0x8; first.carrier = 1234;
        t.onEvent(first, 18.0); t.onEvent(first, 18.2);
        CHECK(voiceVerdict(t.slotState(1, 18.5)) == VoiceVerdict::EncryptedNotDecoded, "D-CONNECT saying encrypted: refused");
    }
    {   // a long group call: D-TX-GRANTED / CEASED of the same call keep a clear call alive past kTtlSec; they never make a slot clear on their own
        CallTracker t;
        t.setMainCarrier(1234);
        CallEvent tx; tx.kind = 4; tx.callId = 300;
        t.onEvent(tx, 5.0);
        CHECK(!t.slotState(3, 5.5).known, "D-TX-GRANTED of an unknown call: nothing");
        CallEvent su; su.kind = 1; su.callId = 300; su.enc = 0; su.slots = 0x2; su.carrier = 1234;   // timeslot 3
        t.onEvent(su, 10.0);
        t.onEvent(tx, 25.0);   // the next talk spurt, 15 s after the set-up
        CHECK(t.slotState(3, 10.0 + CallTracker::kTtlSec + 5).known && t.slotState(3, 35.0).encMode == 0, "the call goes on: still clear after kTtlSec");
        CHECK(!t.slotState(3, 25.0 + CallTracker::kTtlSec + 1).known, "no talk spurt for kTtlSec: it expires");
        CallEvent otherTx = tx; otherTx.callId = 301;
        t.onEvent(otherTx, 40.0);
        CHECK(!t.slotState(3, 50.0).known, "another call's D-TX-GRANTED does not keep it");
        CallEvent enc = su; enc.callId = 400; enc.enc = 1; enc.slots = 0x1;   // timeslot 4, encrypted
        t.onEvent(enc, 60.0); t.onEvent(enc, 61.0);
        tx.callId = 400;
        t.onEvent(tx, 75.0);
        CHECK(voiceVerdict(t.slotState(4, 85.0)) == VoiceVerdict::EncryptedNotDecoded, "an encrypted call stays encrypted");
    }
    {   // the whole path with a fake plug-in: clear call -> both frames reach the codec; encrypted, unknown, or cell-encrypted -> nothing
        const auto A = randomFrame(g), B = randomFrame(g);
        const uint32_t scr = scramblerInit(262, 1, 5);
        std::vector<float> l;
        makeBurst(A.data(), B.data(), scr, l);
        auto run = [&](const CallEncryption& e, std::vector<std::vector<uint8_t>>& frames) {
            TetraVoiceSink vs;
            vs.setCodec(std::make_unique<Recorder>(&frames));
            vs.setFrameDecoder(makeTrafficFrameDecoder());
            size_t pcm = 0;
            vs.setPcmOut([&pcm](const int16_t*, size_t n, int) { pcm += n; });
            const VoiceVerdict v = processTrafficBurst(l.data(), l.data() + 216, scr, e, &vs);
            return std::make_pair(v, pcm);
        };
        {   std::vector<std::vector<uint8_t>> fr; CallEncryption e; e.known = true;
            auto r = run(e, fr);
            CHECK(r.first == VoiceVerdict::Passed && fr.size() == 2 && r.second == 480, "clear call: two frames, two PCM blocks (%zu frames)", fr.size());
            CHECK(fr.size() == 2 && fr[0] == A && fr[1] == B, "the codec gets the 137 bit frames exactly as coded (A then B)");
        }
        {   std::vector<std::vector<uint8_t>> fr; CallEncryption e; e.known = true; e.encMode = 1;
            auto r = run(e, fr);
            CHECK(r.first == VoiceVerdict::EncryptedNotDecoded && fr.empty() && r.second == 0, "encrypted call: no speech frames");
        }
        {   std::vector<std::vector<uint8_t>> fr; CallEncryption e;   // known = false
            auto r = run(e, fr);
            CHECK(r.first == VoiceVerdict::StateUnknownNotDecoded && fr.empty() && r.second == 0, "no set-up seen: no speech frames");
        }
        {   std::vector<std::vector<uint8_t>> fr; CallEncryption e; e.known = true; e.cellOffersEncryption = true;
            auto r = run(e, fr);
            CHECK(r.first == VoiceVerdict::Passed, "cell only advertising air interface encryption, call known clear: decoded");
        }
        {   // a wrong colour code descrambles to noise: the CRC drops the burst, nothing reaches the codec
            std::vector<std::vector<uint8_t>> fr; CallEncryption e; e.known = true;
            TetraVoiceSink vs; vs.setCodec(std::make_unique<Recorder>(&fr)); vs.setFrameDecoder(makeTrafficFrameDecoder());
            vs.setPcmOut([](const int16_t*, size_t, int) {});
            processTrafficBurst(l.data(), l.data() + 216, scramblerInit(262, 1, 6), e, &vs);
            CHECK(fr.empty() || fr[0] != A, "wrong colour code: no valid speech (the CRC drops the burst)");
        }
    }
    return testSummary("tetra traffic");
}
