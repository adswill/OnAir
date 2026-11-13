// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL frame version 4 with the real voice codec (KAAL voice codec 4 through kv4SlotCodec): four talkers, one per slot, at the four
// profiles, with pauses (DTX gives the slot to data). Through the PHY at a good signal the decoded speech of every slot is bit-identical
// to the codec decoding its own output (the PHY is transparent); at lower signals the payload loss and the speech SNR against that
// reference are printed.
#include "kaal_v4_test_common.h"
#include <cstdio>
#include <cstring>

using namespace kv4;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int16_t talker(int s, size_t n) {   // speech-like: a voiced harmonic sound with a gliding pitch, syllables and pauses
    const double t = n / 16000.0;
    const double f0 = 110 + 40 * s + 25 * std::sin(2 * kPi * (0.7 + 0.2 * s) * t);
    const double syl = std::sin(2 * kPi * (3.0 + 0.5 * s) * t);
    const bool pause = std::fmod(t + 0.4 * s, 2.5) > 1.8;   // 0.7 s of silence every 2.5 s
    if (pause || syl < -0.2) return 0;
    double v = 0;
    for (int h = 1; h <= 12; h++) v += std::sin(2 * kPi * f0 * h * t + h) / (h * (1 + 0.15 * h * std::fabs(std::sin(2 * kPi * 0.5 * t))));
    return (int16_t)(6000 * v * std::min(1.0, syl + 0.2));
}

struct Run { int sent = 0, voiceOk = 0, voiceLost = 0; double snrDb[kSlots] = {}; bool identical[kSlots] = {}; };

static Run run(double esn0, int nSf, uint32_t seed) {
    const Profile prof[kSlots] = {kV80, kV60, kV48, kV32R};
    const SlotCodec& kc = kv4SlotCodec();
    std::unique_ptr<SlotVoiceEncoder> enc[kSlots];
    for (int s = 0; s < kSlots; s++) enc[s].reset(kc.makeEncoder());
    FrameTx tx;
    tx.configure(kFs);
    for (int s = 0; s < kSlots; s++) tx.setSlotProfile(s, prof[s]);
    std::vector<std::array<std::array<uint8_t, 40>, kSlots>> pay(nSf);
    std::vector<std::array<bool, kSlots>> has(nSf);
    std::vector<cf> sig;
    int16_t pcm[kPcmPerPayload];
    size_t n0 = 0;
    for (int n = 0; n < nSf; n++) {
        for (int s = 0; s < kSlots; s++) {
            for (int i = 0; i < kPcmPerPayload; i++) pcm[i] = talker(s, n0 + i);
            pay[n][s].fill(0);
            has[n][s] = enc[s]->encode(pcm, prof[s], pay[n][s].data());
            if (has[n][s]) tx.pushVoice(s, pay[n][s].data());
        }
        n0 += kPcmPerPayload;
        tx.superframe(sig);
    }
    Chan c; c.esn0Db = esn0; c.cfoHz = 700; c.lead = 900; c.seed = seed;
    const std::vector<cf> y = channel(sig, c);
    FrameRx rx;
    rx.configure(kFs);
    std::vector<RxSuperframe> got;
    for (size_t p = 0; p < y.size(); p += 4800) {
        rx.feed(y.data() + p, std::min<size_t>(4800, y.size() - p));
        RxSuperframe r;
        while (rx.popSuperframe(r)) got.push_back(r);
    }
    Run out;
    // align: the first superframe whose slot 0 decoded as voice, matched by content
    int off = -1, first = -1;
    for (size_t g = 0; g < got.size() && off < 0; g++)
        if (got[g].slot[0].ok && profileVoice(got[g].slot[0].profile))
            for (int n = 0; n < nSf; n++) if (has[n][0] && !std::memcmp(pay[n][0].data(), got[g].slot[0].bits, 40)) { off = n - (int)g; first = (int)g; break; }
    if (off < 0) return out;
    for (int s = 0; s < kSlots; s++) {
        std::unique_ptr<SlotVoiceDecoder> dref(kc.makeDecoder()), drx(kc.makeDecoder());
        double es = 0, ee = 0;
        bool same = true;
        for (size_t g = (size_t)first; g < got.size(); g++) {
            const int n = (int)g + off;
            if (n >= nSf) break;
            int16_t a[kPcmPerPayload], b[kPcmPerPayload];
            if (has[n][s]) dref->decode(pay[n][s].data(), prof[s], a); else dref->silence(a);
            const SlotPayload& p = got[g].slot[s];
            if (has[n][s]) out.sent++;
            if (p.ok && profileVoice(p.profile)) { drx->decode(p.bits, p.profile, b); out.voiceOk++; }
            else if (p.ok) drx->silence(b);
            else { drx->decode(nullptr, prof[s], b); if (has[n][s]) out.voiceLost++; }
            for (int i = 0; i < kPcmPerPayload; i++) { es += (double)a[i] * a[i]; const double d = (double)a[i] - b[i]; ee += d * d; if (a[i] != b[i]) same = false; }
        }
        out.snrDb[s] = 10 * std::log10((es + 1) / (ee + 1e-3));
        out.identical[s] = same;
    }
    return out;
}

int main() {
    const char* names[kSlots] = {"V80", "V60", "V48", "V32R"};
    {
        const Run r = run(25, 125, 1);   // 5 s
        printf("Es/N0 25 dB: %d voice payloads sent, %d received, %d lost\n", r.sent, r.voiceOk, r.voiceLost);
        for (int s = 0; s < kSlots; s++) {
            printf("  slot %d %s: decoded speech %s the codec's own decode (SNR %.1f dB)\n", s, names[s], r.identical[s] ? "identical to" : "differs from", r.snrDb[s]);
            CHECK(r.identical[s], "slot %d not bit-identical", s);
        }
        CHECK(r.voiceLost == 0 && r.sent > 300, "payloads %d lost %d", r.sent, r.voiceLost);
    }
    for (double e : {6.0, 7.0, 8.0, 10.0}) {
        const Run r = run(e, 125, 2);
        printf("Es/N0 %4.1f dB: voice payload loss %.3f; speech SNR vs the codec's own decode:", e, r.sent ? (double)r.voiceLost / r.sent : 1.0);
        for (int s = 0; s < kSlots; s++) printf(" %s %.1f", names[s], r.snrDb[s]);
        printf(" dB\n");
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("all ok\n");
    return 0;
}
