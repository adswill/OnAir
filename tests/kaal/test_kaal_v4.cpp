// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL frame version 4: slot and control word round trips, layered LDPC, clean loopback with four slots, text, beacon, lock time,
// no false lock on noise or on a v3 signal (docs/KAAL-NEXT.md).
#include "kaal_v4_test_common.h"
#include "dect2/kaal/kaal_fec.h"
#include "dect2/kaal/kaal_tx.h"
#include <cstdio>
#include <cstring>

using namespace kv4;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void unitSlots() {
    std::mt19937 rng(5);
    for (int pi = 0; pi < kProfiles; pi++) {
        const Profile p = (Profile)pi;
        for (int s = 0; s < kSlots; s++) {
            SlotPayload in;
            in.profile = p;
            in.nbits = (uint16_t)payloadBits(p);
            for (auto& b : in.bits) b = (uint8_t)rng();
            if (profileVoice(p)) for (int i = payloadBits(p); i < 320; i++) in.bits[i >> 3] &= (uint8_t)~(0x80 >> (i & 7));
            in.dataType = p == kIdle ? 0 : 1; in.stream = 3; in.seq = 77;
            if (p == kIdle) std::memset(in.bits, 0, sizeof(in.bits));
            const std::vector<uint8_t> c = encodeSlot(s, in);
            CHECK((int)c.size() == (profileQpsk(p) ? kSlotCodedQ : kSlotCoded16), "coded size");
            std::vector<float> llr(c.size());
            for (size_t i = 0; i < c.size(); i++) llr[i] = c[i] ? -4.f : 4.f;
            SlotPayload out;
            const bool ok = decodeSlot(s, llr.data(), p == kIdle ? kD68 : p, out);
            CHECK(ok, "profile %d slot %d decodes", pi, s);
            if (profileVoice(p)) CHECK(std::memcmp(in.bits, out.bits, 40) == 0, "voice bits %d", pi);
            else {
                const int nb = dataBytes(p);
                CHECK(std::memcmp(in.bits, out.bits, nb) == 0 && (p == kIdle || (out.seq == 77 && out.stream == 3)), "data %d", pi);
                CHECK(out.profile == p, "profile back %d -> %d", pi, out.profile);
            }
            // every other hypothesis must fail (profile from the code length)
            for (int qi = 0; qi < kProfiles; qi++) {
                const Profile q = (Profile)qi;
                if (q == p || q == kIdle || (p == kIdle && q == kD68) || profileQpsk(q) != profileQpsk(p)) continue;
                SlotPayload o2;
                CHECK(!decodeSlot(s, llr.data(), q, o2), "profile %d accepted as %d", pi, qi);
            }
            // wrong slot (scrambler) must fail
            SlotPayload o3;
            CHECK(!decodeSlot((s + 1) % kSlots, llr.data(), p == kIdle ? kD68 : p, o3) || p == kIdle, "slot %d word accepted in slot %d", s, (s + 1) % 4);
        }
    }
    ControlWord w;
    w.map[0] = kV60; w.map[1] = kV80; w.map[2] = kD68; w.map[3] = kV32R;
    w.msgType = kMsgText; w.part = 2; w.parts = 5;
    for (int i = 0; i < kCtrlMsgBytes; i++) w.msg[i] = (uint8_t)(i * 7 + 1);
    const std::vector<uint8_t> c = encodeControl(w);
    std::vector<float> llr(c.size());
    for (size_t i = 0; i < c.size(); i++) llr[i] = c[i] ? -3.f : 3.f;
    ControlWord o;
    CHECK(decodeControl(llr.data(), o), "control decodes");
    CHECK(o.version == 4 && o.map[1] == kV80 && o.map[3] == kV32R && o.part == 2 && o.parts == 5 && std::memcmp(o.msg, w.msg, kCtrlMsgBytes) == 0, "control fields");
}

static void unitLayered() {
    // same decisions as the flooding decoder on noisy words, fewer iterations
    std::mt19937 rng(9);
    std::normal_distribution<float> nd(0, 1);
    int okF = 0, okL = 0, itF = 0, itL = 0;
    for (int t = 0; t < 200; t++) {
        uint8_t info[320], cw[640], o[320];
        for (auto& b : info) b = rng() & 1;
        dect2::kaal::ldpcEncode(info, cw);
        float llr[640];
        const float sigma = 0.80f;   // BPSK, Eb/N0 about 1.9 dB at rate 1/2
        for (int i = 0; i < 640; i++) llr[i] = 2 * ((cw[i] ? -1.f : 1.f) + sigma * nd(rng)) / (sigma * sigma);
        int i1 = 0, i2 = 0;
        if (dect2::kaal::ldpcDecode(llr, o, 40, 0.8f, &i1) && std::memcmp(o, info, 320) == 0) okF++;
        if (dect2::kaal::ldpcDecodeLayered(llr, o, 40, 0.9f, nullptr, &i2) && std::memcmp(o, info, 320) == 0) okL++;
        itF += i1; itL += i2;
    }
    printf("LDPC (640,320) BPSK Eb/N0 1.9 dB, 200 words: flooding %d ok (mean %.1f it), layered %d ok (mean %.1f it)\n", okF, itF / 200.0, okL, itL / 200.0);
    CHECK(okL >= okF - 3, "layered not worse");
    CHECK(itL < itF, "layered converges faster");
}

static void loopClean() {
    const Profile prof[kSlots] = {kV60, kV80, kV48, kV32R};
    Sent sent;
    const std::string text = "KAAL v4 text: four voice slots and data in one 20 kHz channel. 73 de LY2ABC";
    std::vector<cf> sig = makeSignal(prof, 60, sent, 11, text, true);
    Chan c;
    c.esn0Db = 30; c.lead = 1234; c.cfoHz = 870; c.delay = 0.37;
    const Score sc = runRx(channel(sig, c), sent, 3);
    int tot = 0, good = 0, wrong = 0;
    for (int s = 0; s < kSlots; s++) { tot += sc.sent[s]; good += sc.good[s]; wrong += sc.wrong[s]; }
    printf("clean loopback (Es/N0 30 dB, cfo 870 Hz): %d/%d voice payloads, %d wrong, lock %.3f s, Es/N0 est %.1f dB, cfo est %.1f Hz, beacons %d, texts %zu\n",
           good, tot, wrong, sc.lockS, sc.esn0, sc.cfo, sc.beacons, sc.texts.size());
    CHECK(good == tot && wrong == 0, "all payloads");
    CHECK(sc.lockS > 0 && sc.lockS < 0.25, "lock within 0.25 s (%.3f)", sc.lockS);
    CHECK(std::fabs(sc.cfo - 870) < 5, "cfo %.1f", sc.cfo);
    CHECK(sc.beacons >= 1, "beacon in the control channel");
    // the text went with all four slots busy: through the control channel
    bool found = false;
    for (auto& t : sc.texts) if (t == text) found = true;
    CHECK(found, "text through the control channel (%zu texts)", sc.texts.size());

    // data slots: text through free slots, fast
    const Profile prof2[kSlots] = {kV60, kIdle, kIdle, kIdle};
    Sent s2;
    std::string big;
    for (int i = 0; i < 30; i++) big += "The quick brown fox jumps over the lazy dog " + std::to_string(i) + ". ";
    sig = makeSignal(prof2, 40, s2, 12, big, false);
    Chan c2; c2.esn0Db = 25; c2.lead = 500;
    const Score sc2 = runRx(channel(sig, c2), s2, 0);
    found = false;
    for (auto& t : sc2.texts) if (t == big) found = true;
    printf("text %zu bytes in 3 data slots: received %s (%zu texts), voice %d/%d\n", big.size(), found ? "yes" : "no", sc2.texts.size(), sc2.good[0], sc2.sent[0]);
    CHECK(found, "big text in data slots");
}

static void noFalseLock() {
    // noise only
    std::vector<cf> n(48000 * 3, cf(0, 0));
    Chan c; c.esn0Db = 0; c.seed = 3;
    // noise at a level as if a signal of rms 0.25 were at 0 dB
    std::mt19937 rng(4);
    std::normal_distribution<float> nd(0, 0.25f);
    for (auto& v : n) v = cf(nd(rng), nd(rng));
    FrameRx rx; rx.configure(kFs);
    rx.feed(n.data(), n.size());
    RxStatsV4 st = rx.stats();
    printf("noise 3 s: locks %llu, slots ok %llu\n", (unsigned long long)st.locks, (unsigned long long)st.slotsOk);
    CHECK(st.slotsOk == 0, "no slot from noise");
    // a v3 signal (16 kbit/s voice, sweep, beacon)
    dect2::kaal::VoiceTx v3;
    v3.configure(48000);
    v3.setBitrate(16);
    v3.setSweepCallsign("LY2ABC");
    std::vector<int16_t> pcm(16000 * 3);
    for (size_t i = 0; i < pcm.size(); i++) pcm[i] = (int16_t)(8000 * std::sin(i * 0.07) * std::sin(i * 0.0011));
    v3.pushAudio(pcm.data(), pcm.size());
    v3.flush();
    std::vector<cf> s3(v3.available());
    s3.resize(v3.popIq(s3.data(), s3.size()));
    FrameRx rx2; rx2.configure(kFs);
    rx2.feed(s3.data(), s3.size());
    st = rx2.stats();
    printf("v3 signal %.1f s into the v4 receiver: locks %llu, slots ok %llu\n", s3.size() / 48000.0, (unsigned long long)st.locks, (unsigned long long)st.slotsOk);
    CHECK(st.slotsOk == 0, "no slot from a v3 signal");
}

int main() {
    unitSlots();
    unitLayered();
    loopClean();
    noFalseLock();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("all ok\n");
    return 0;
}
