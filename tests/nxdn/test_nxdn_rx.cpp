// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NXDN receiver against the test signal with real-world faults: both rates, +-1 kHz offset, off centre with clock errors, noise, symbol
// errors, a call joined late, a call without its release, all at once; and noise only. Silent.
#include "dect2/nxdn/nxdn_gen.h"
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/nxdn/nxdn_tel.h"
#include "dect2/nxdn/nxdn_proto.h"
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
    int baudOnly = 0;            // 0 both, else only that rate's transmissions
    bool late = false, noRel = false;
};

static NxdnTelemetry run(const Case& k, std::vector<NxdnGenTx> txs, int* voiceOk = nullptr) {
    NxdnGenConfig c;
    c.rate = 500000; c.dialOffsetHz = k.dial; c.snrDb = k.snr; c.cfoHz = k.cfo; c.clockPpm = k.ppm; c.symErrRate = k.symErr; c.loop = false; c.seed = 3;
    for (auto& t : txs) {
        if (k.baudOnly && t.baud != k.baudOnly) continue;
        if (t.kind == NxdnGenTx::Voice) { t.noHeader = k.late; t.noRelease = k.noRel; }
        c.txs.push_back(t);
    }
    NxdnGenerator g(c);
    NxdnReceiver rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    rx.setSignalOffset(k.dial);
    int vok = 0; uint32_t vfn = 0;
    rx.setVoiceTap([&](const uint8_t b[72]) {   // the voice frames come in order; count those received without an error
        uint8_t want[72];
        for (uint32_t t = 0; t < 400; t++) { nxdnKnownVoice(t, want); if (!memcmp(want, b, 72)) { vok++; vfn = t + 1; return; } }
        vfn++;
    });
    double total = 0.5;
    for (const auto& t : c.txs) total += (t.kind == NxdnGenTx::Voice || t.kind == NxdnGenTx::Site ? t.durSec + 0.4 : 0.4) + t.gapSec;
    std::vector<cf32> buf(25000);
    for (double s = 0; s < total + 1.0; s += buf.size() / c.rate) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    NxdnTelemetry t;
    rx.telemetry(t, 0);
    if (voiceOk) *voiceOk = vok;
    return t;
}

static const NxdnCall* find(const NxdnTelemetry& t, int kind, int src) {
    for (const auto& c : t.log) if (c.kind == kind && c.src == src) return &c;
    return nullptr;
}

int main() {
    auto sched = nxdnTestSchedule();
    if (getenv("NXDN_ONLY")) sched = {sched[(size_t)atoi(getenv("NXDN_ONLY"))]};
    std::vector<Case> cases;
    cases.push_back({"clean"});
    { Case k{"NXDN48 +1 kHz"}; k.cfo = 1000; k.baudOnly = 2400; cases.push_back(k); }
    { Case k{"NXDN48 -1 kHz"}; k.cfo = -1000; k.baudOnly = 2400; cases.push_back(k); }
    { Case k{"NXDN96 +1 kHz"}; k.cfo = 1000; k.baudOnly = 4800; cases.push_back(k); }
    { Case k{"NXDN96 -1 kHz"}; k.cfo = -1000; k.baudOnly = 4800; cases.push_back(k); }
    { Case k{"off centre, +150 ppm clock"}; k.dial = 60000; k.ppm = 150; cases.push_back(k); }
    { Case k{"-150 ppm clock"}; k.ppm = -150; cases.push_back(k); }
    { Case k{"noise 16 dB"}; k.snr = 16; cases.push_back(k); }
    { Case k{"symbol errors 1 %"}; k.symErr = 0.01; cases.push_back(k); }
    { Case k{"joined late, no release"}; k.late = true; k.noRel = true; cases.push_back(k); }
    { Case k{"all at once"}; k.cfo = -800; k.dial = -40000; k.ppm = 100; k.snr = 18; k.symErr = 0.005; k.late = true; cases.push_back(k); }
    for (const auto& k : cases) {
        int vok = 0;
        const NxdnTelemetry t = run(k, sched, &vok);
        const bool want48 = k.baudOnly != 4800, want96 = k.baudOnly != 2400;
        const NxdnCall* v48 = find(t, 0, 1001);
        const NxdnCall* st = find(t, 1, 1002);
        const NxdnCall* sd = find(t, 2, 2005);
        const NxdnCall* v96 = find(t, 0, 2001);
        printf("  %-28s frames %llu, CRC ok %llu bad %llu, BER %.2f %%, SNR %.1f dB, cfo %+.0f Hz, %zu log, voice %d/%llu, site %s\n", k.name,
               (unsigned long long)t.frames, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.berPct, t.snrDb, t.cfoHz, t.log.size(), vok,
               (unsigned long long)t.voiceFrames, t.siteInfo.valid ? "yes" : "no");
        if (want48) {
            CHECK(v48 && v48->dst == 200 && v48->callType == 1 && v48->ran == 1 && v48->baud == 2400, "%s: NXDN48 group call", k.name);
            if (v48) CHECK(v48->endSeen == !k.noRel && v48->lateEntry == k.late, "%s: NXDN48 end %d late %d", k.name, v48->endSeen, v48->lateEntry);
            if (!st || getenv("NXDN_EV")) for (const auto& s : t.events) printf("    event: %s\n", s.c_str());
            CHECK(st && st->dst == 1001 && st->text == "status 17" && st->callType == 4, "%s: status", k.name);
        }
        if (want96) {
            CHECK(v96 && v96->dst == 2002 && v96->callType == 4 && v96->baud == 4800, "%s: NXDN96 individual call", k.name);
            CHECK(sd && sd->dst == 2001 && sd->text == "HELLO FROM NXDN96", "%s: short data '%s'", k.name, sd ? sd->text.c_str() : "-");
            CHECK(t.siteInfo.valid && t.siteInfo.location == 0x0A4C2B && t.siteInfo.ran == 5, "%s: site info", k.name);
        }
        // the voice frames: all of them when the channel is clean
        if (k.symErr == 0 && k.snr >= 30) CHECK(vok > 0 && (uint64_t)vok == t.voiceFrames, "%s: voice frames %d of %llu", k.name, vok, (unsigned long long)t.voiceFrames);
    }
    // noise only: nothing found
    {
        Case k{"noise only"}; k.snr = 40;
        std::vector<NxdnGenTx> none;
        NxdnGenTx gap; gap.kind = NxdnGenTx::Status; gap.baud = 2400;
        NxdnGenConfig c; c.rate = 500000; c.snrDb = 10; c.loop = false;
        NxdnGenerator g(c);   // no transmissions: noise only
        NxdnReceiver rx; rx.setSilent(true); rx.configure(c.rate);
        std::vector<cf32> buf(25000);
        for (int i = 0; i < 200; i++) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
        NxdnTelemetry t; rx.telemetry(t, 0);
        printf("  noise only: %llu frames tried, %llu CRC ok, %zu log\n", (unsigned long long)t.frames, (unsigned long long)t.blocksOk, t.log.size());
        CHECK(t.blocksOk == 0 && t.log.empty() && t.state == 0, "noise only");
    }
    printf(fails ? "nxdn rx: %d FAILED\n" : "nxdn rx: all passed\n", fails);
    return fails ? 1 : 0;
}
