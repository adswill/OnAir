// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL frame version 4 through the transmit generator (opt kaal.frame 4, the TX and TX-to-IQ-file path) at a radio rate of 240 kHz with a carrier
// offset into KaalLiveReceiver (what the engine feeds): v4 lock, the beacon from the control channel, a 300-character text from the data slots,
// and the voice of slot 0 through a stub slot codec (the real codec registers itself the same way, kaal_slots.h).
#include "dect2/kaal/kaal_live.h"
#include "dect2/kaal/kaal_tel.h"
#include "dect2/kaal/kaal_txgen.h"
#include "dect2/kaal/kaal_v4.h"
#include "dect2/common/ring.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

namespace {
using namespace dect2::kaal::v4;
int encoded = 0, decodedOk = 0, decodedLost = 0;
struct StubEnc : SlotVoiceEncoder {
    uint8_t n = 0;
    bool encode(const int16_t*, Profile p, uint8_t* packed) override {
        for (int i = 0; i < (payloadBits(p) + 7) / 8; i++) packed[i] = (uint8_t)(n + i * 13);
        n++; encoded++;
        return true;
    }
};
struct StubDec : SlotVoiceDecoder {
    void decode(const uint8_t* packed, Profile, int16_t* pcm) override {
        if (packed) decodedOk++; else decodedLost++;
        for (int i = 0; i < kPcmPerPayload; i++) pcm[i] = (int16_t)(packed ? 1000 : 0);
    }
};
SlotVoiceEncoder* mkE() { return new StubEnc; }
SlotVoiceDecoder* mkD() { return new StubDec; }
bool sup(Profile p) { return profileVoice(p); }
}  // namespace

int main() {
    registerSlotCodec({"stub", mkE, mkD, sup});
    const double rate = 240000;
    TxRequest r;
    r.modeId = "kaal";
    r.callsign = "ly2abc";
    r.grid = "KO24pq";
    r.lat = 54.687; r.lon = 25.279;
    for (int i = 0; i < 6; i++) r.text += "KAAL v4 sends long texts in the data slots, " + std::to_string(i) + ". ";
    r.opt["kaal.frame"] = 4;
    r.opt["kaal.v4voice"] = 1;
    r.opt["kaal.v4profile"] = 2;
    r.opt["kaal.powerDbm"] = 30;
    r.pullAudio = [&](float* st, int n) { for (int i = 0; i < 2 * n; i++) st[i] = 0.05f * (float)std::sin(i * 0.01); return n; };
    std::string err;
    auto gen = makeKaalTxGenerator(r, rate, err);
    CHECK(gen != nullptr, "generator: %s", err.c_str());
    if (!gen) return testSummary("kaal_v4_live");
    { TxRequest q = r; q.opt["kaal.v4profile"] = 7; CHECK(!makeKaalTxGenerator(q, rate, err) && !err.empty(), "profile 7 refused"); }

    KaalLiveReceiver rx;
    rx.setSilent(true);
    size_t tap = 0;
    rx.setAudioTap([&](const int16_t*, size_t n) { tap += n; });
    rx.configure(rate);
    KaalTelemetry last;
    uint64_t seq = 0;
    std::vector<cf32> blk(24000);
    double ph = 0;
    for (int i = 0; i < 40; i++) {   // 4 s
        gen->generate(blk.data(), blk.size());
        for (auto& v : blk) { v *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += 2 * 3.141592653589793 * 1500.0 / rate; }
        rx.feed(blk.data(), blk.size());
        KaalTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; last = t; }
    }
    printf("state %d frame v%d, slots ok %llu bad %llu, Es/N0 %.1f dB, cfo %.1f Hz, beacon %d '%s', texts %llu, slot 0 profile %d call '%s', tap %zu, enc %d dec %d/%d\n",
           last.state, last.frameVersion, (unsigned long long)last.v4SlotsOk, (unsigned long long)last.v4SlotsBad, (double)last.snrDb, last.cfoHz, (int)last.beaconValid,
           last.callsign.c_str(), (unsigned long long)last.v4Texts, last.v4Profile[0], last.v4Call[0].c_str(), tap, encoded, decodedOk, decodedLost);
    CHECK(last.state == 2 && last.frameVersion == 4, "v4 locked");
    CHECK(std::fabs(last.cfoHz - 1500) < 20, "carrier %.1f", last.cfoHz);
    CHECK(last.beaconValid && last.callsign == "LY2ABC" && last.grid == "KO24pq", "beacon");
    CHECK(last.text == r.text, "text '%s'", last.text.c_str());
    CHECK(last.v4Profile[0] == kV60 && last.v4Call[0] == "LY2ABC", "slot 0");
    CHECK(decodedOk >= 80 && tap >= 80u * kPcmPerPayload, "voice of slot 0 decoded %d", decodedOk);
    CHECK(last.v4SlotsBad <= 4, "bad slots %llu", (unsigned long long)last.v4SlotsBad);
    return testSummary("kaal_v4_live");
}
