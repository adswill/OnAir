// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// dPMR receiver against the test signal with real-world faults: +-1 kHz offset, off centre with clock errors, noise, symbol errors, a call
// joined late, a call without its end frame, all at once, scanning the 16 PMR446 channels; and noise only.
// Silent.
#include "dect2/dpmr/dpmr_gen.h"
#include "dect2/dpmr/dpmr_rx.h"
#include "dect2/dpmr/dpmr_tel.h"
#include "dect2/dpmr/dpmr_proto.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Case {
    const char* name;
    double cfo = 0, ppm = 0, snr = 40, symErr = 0, dial = 0;
    bool late = false, noEnd = false, scan = false;
};

static DpmrTelemetry run(const Case& k, std::vector<DpmrGenTx> txs, int* voiceOk = nullptr) {
    DpmrGenConfig c;
    c.rate = 500000; c.dialOffsetHz = k.dial; c.snrDb = k.snr; c.cfoHz = k.cfo; c.clockPpm = k.ppm; c.symErrRate = k.symErr; c.loop = k.scan; c.seed = 3;
    for (auto& t : txs) {
        if (t.kind == DpmrGenTx::Voice) { t.noHeader = k.late; t.noEnd = k.noEnd; }
        if (k.scan) t.chOffsetHz = 8 * 6250.0;   // PMR446 channel 9 while the receiver starts on channel 1
        c.txs.push_back(t);
    }
    DpmrGenerator gsig(c);
    DpmrReceiver rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    rx.setSignalOffset(k.dial);
    rx.setChannel(dpmr::pmr446Mhz(1));
    rx.setScan(k.scan);
    int vok = 0;
    rx.setVoiceTap([&](const uint8_t b[72]) {
        uint8_t want[72];
        for (uint32_t t = 0; t < 400; t++) { dpmrKnownVoice(t, want); if (!memcmp(want, b, 72)) { vok++; return; } }
    });
    double total = 0.5 + (k.scan ? 9.0 : 0);
    for (const auto& t : c.txs) total += (t.kind == DpmrGenTx::Voice ? t.durSec + 0.4 : 0.6) + t.gapSec;
    std::vector<cf32> buf(25000);
    for (double s = 0; s < total + 1.5; s += buf.size() / c.rate) { gsig.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    DpmrTelemetry t;
    rx.telemetry(t, 0);
    if (voiceOk) *voiceOk = vok;
    return t;
}

static const DpmrCall* find(const DpmrTelemetry& t, int kind, int64_t src) {
    for (const auto& c : t.log) if (c.kind == kind && c.src == src) return &c;
    return nullptr;
}

int main() {
    auto sched = dpmrTestSchedule();
    std::vector<Case> cases;
    cases.push_back({"clean"});
    { Case k{"+1 kHz"}; k.cfo = 1000; cases.push_back(k); }
    { Case k{"-1 kHz"}; k.cfo = -1000; cases.push_back(k); }
    { Case k{"off centre +150 ppm"}; k.dial = 120000; k.ppm = 150; cases.push_back(k); }
    { Case k{"-150 ppm"}; k.ppm = -150; cases.push_back(k); }
    { Case k{"16 dB noise"}; k.snr = 16; cases.push_back(k); }
    { Case k{"1 % symbol errors"}; k.symErr = 0.01; cases.push_back(k); }
    { Case k{"late entry"}; k.late = true; cases.push_back(k); }
    { Case k{"no end frame"}; k.noEnd = true; cases.push_back(k); }
    { Case k{"all at once"}; k.cfo = 900; k.ppm = 100; k.snr = 18; k.symErr = 0.005; k.late = true; k.dial = -80000; cases.push_back(k); }
    { Case k{"scan to channel 9"}; k.scan = true; cases.push_back(k); }
    for (const Case& k : cases) {
        int vok = 0;
        const DpmrTelemetry t = run(k, sched, &vok);
        const DpmrCall* v1 = find(t, 0, 1001);
        const DpmrCall* st = find(t, 1, 1002);
        const DpmrCall* sm = find(t, 2, 2005);
        const DpmrCall* pk = find(t, 3, 2006);
        const DpmrCall* v2 = find(t, 0, 2001);
        printf("  %-22s frames %llu, CRC ok %llu bad %llu, BER %.2f %%, SNR %.1f dB, cfo %+.0f Hz, %zu log, voice %d/%llu, ch %d\n", k.name, (unsigned long long)t.frames,
               (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.berPct, t.snrDb, t.cfoHz, t.log.size(), vok, (unsigned long long)t.voiceFrames, t.channel);
        if (k.scan) {   // the schedule plays in a loop on channel 9: the scan must stop there and decode what follows
            CHECK(t.channel == 9 && t.scanning && t.log.size() >= 3 && t.log[0].channel == 9, "%s: channel %d, %zu log", k.name, t.channel, t.log.size());
            continue;
        }
        if (getenv("DPMR_EV")) for (const auto& s : t.events) printf("    event: %s\n", s.c_str());
        CHECK(v1 && v1->dst == 200 && v1->callType == 1 && v1->cc == 7, "%s: group call", k.name);
        if (v1) CHECK(v1->endSeen == !k.noEnd && v1->lateEntry == k.late, "%s: end %d late %d", k.name, v1->endSeen, v1->lateEntry);
        CHECK(st && st->dst == 1001 && st->text == "status 17" && st->callType == 0, "%s: status", k.name);
        CHECK(sm && sm->dst == 2001 && (sm->text == "HELLO FROM DPMR PMR446 CHANNEL" || (k.symErr > 0 && sm->text.rfind("HELLO", 0) == 0)) && sm->cc == 12, "%s: short message '%s'", k.name, sm ? sm->text.c_str() : "-");
        CHECK(pk && pk->dst == 0xFFFFFF && pk->text == "PACKET DATA" && pk->callType == 2, "%s: packet data", k.name);
        CHECK(v2 && v2->dst == 2002 && v2->callType == 0 && v2->emergency, "%s: individual emergency call", k.name);
        // without an end frame the call ends on carrier loss, and up to a few trailing noise frames are counted as voice (not CRC ok) first;
        // their number depends on the noise realisation, so the no-end case allows up to 6 of them
        if (k.symErr == 0 && k.snr >= 30) CHECK(vok > 0 && (int64_t)t.voiceFrames >= vok && (int64_t)t.voiceFrames - vok <= (k.noEnd ? 6 : 0), "%s: voice frames %d of %llu", k.name, vok, (unsigned long long)t.voiceFrames);
    }
    {
        DpmrGenConfig c; c.rate = 500000; c.snrDb = 10; c.loop = false;
        DpmrGenerator gsig(c);
        DpmrReceiver rx; rx.setSilent(true); rx.configure(c.rate); rx.setChannel(dpmr::pmr446Mhz(1)); rx.setScan(true);
        std::vector<cf32> buf(25000);
        for (int i = 0; i < 200; i++) { gsig.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
        DpmrTelemetry t; rx.telemetry(t, 0);
        printf("  noise only: %llu frames tried, %llu CRC ok, %zu log\n", (unsigned long long)t.frames, (unsigned long long)t.blocksOk, t.log.size());
        CHECK(t.blocksOk == 0 && t.log.empty() && t.state == 0, "noise only");
    }
    printf(fails ? "dpmr rx: %d FAILED\n" : "dpmr rx: all passed\n", fails);
    return fails ? 1 : 0;
}
