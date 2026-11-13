// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// P25 Phase 1 receiver against the test signal with real-world faults: +-1 kHz offset, off centre with a 100 ppm clock error, 14 dB SNR,
// a call joined late and one without its terminator, all at once; an encrypted call is reported as encrypted and never reaches the codec;
// noise only gives nothing. Silent.
#include "dect2/p25/p25_gen.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/p25/p25_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Case {
    const char* name;
    double cfo = 0, ppm = 0, snr = 40, dial = 0;
    bool late = false, noTerm = false;
};

struct Run { P25Telemetry t; int vok = 0, vframes = 0, encToCodec = 0; };

static Run run(const Case& k, const std::vector<P25GenTx>& sched) {
    P25GenConfig c;
    c.rate = 500000; c.snrDb = k.snr; c.cfoHz = k.cfo; c.clockPpm = k.ppm; c.dialOffsetHz = k.dial; c.loop = false;
    for (auto t : sched) { if (t.kind == P25GenTx::Voice) { t.noHeader = k.late; t.noTerm = k.noTerm; } c.txs.push_back(t); }
    P25Generator g(c);
    P25Receiver rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    rx.setSignalOffset(k.dial);
    Run r;
    rx.setVoiceTap([&](const uint8_t* b, bool clear) {
        r.vframes++;
        if (!clear) return;
        uint8_t want[144];
        int best = 999, bt = -1;
        for (uint32_t t = 0; t < 200; t++) {
            p25KnownVoice(t, want);
            int dd = 0; for (int i = 0; i < 144; i++) dd += want[i] != b[i];
            if (dd == 0) { r.vok++; return; }
            if (dd < best) { best = dd; bt = (int)t; }
        }
        if (getenv("P25_EV")) printf("    voice frame %d: nearest %d with %d bit errors\n", r.vframes, bt, best);
    });
    double total = c.leadSec;
    for (const auto& t : c.txs) {
        std::vector<int8_t> s; p25TxSymbols(t, s);
        total += s.size() / 4800.0 + t.gapSec;
    }
    std::vector<cf32> buf(25000);
    for (double s = 0; s < total + 1.5; s += buf.size() / c.rate) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    rx.telemetry(r.t, 0);
    return r;
}

static const P25Call* find(const P25Telemetry& t, int kind, int src, int dst) {
    for (const auto& c : t.log) if (c.kind == kind && (src < 0 || c.src == src) && c.dst == dst) return &c;
    return nullptr;
}

int main() {
    const auto sched = p25TestSchedule();
    std::vector<Case> cases;
    cases.push_back({"clean"});
    { Case k{"+1 kHz"}; k.cfo = 1000; cases.push_back(k); }
    { Case k{"-1 kHz"}; k.cfo = -1000; cases.push_back(k); }
    { Case k{"off centre, 100 ppm"}; k.dial = 30000; k.ppm = 100; cases.push_back(k); }
    { Case k{"-100 ppm"}; k.ppm = -100; cases.push_back(k); }
    { Case k{"14 dB"}; k.snr = 14; cases.push_back(k); }
    { Case k{"joined late"}; k.late = true; cases.push_back(k); }
    { Case k{"no terminator"}; k.noTerm = true; cases.push_back(k); }
    { Case k{"all at once"}; k.cfo = -1000; k.ppm = 100; k.snr = 14; k.dial = -20000; k.late = true; cases.push_back(k); }
    for (const auto& k : cases) {
        const Run r = run(k, sched);
        const P25Telemetry& t = r.t;
        printf("  %-20s %llu frames (%llu NID bad), TSBK %llu/%llu, LC %llu/%llu, %zu calls, SNR %.1f dB, offset %+.0f Hz, voice %d/%d\n", k.name,
               (unsigned long long)t.frames, (unsigned long long)t.nidBad, (unsigned long long)t.tsbkOk, (unsigned long long)t.tsbkBad, (unsigned long long)t.lcOk,
               (unsigned long long)t.lcBad, t.log.size(), t.snrDb, t.cfoHz, r.vok, r.vframes);
        CHECK(t.site.valid && t.site.wacn == 0xBEE00 && t.site.sysId == 0x3A1 && t.site.rfss == 1 && t.site.site == 5 && t.site.nac == 0x293,
              "%s: site WACN %05X system %03X %d-%d NAC %03X", k.name, t.site.wacn, t.site.sysId, t.site.rfss, t.site.site, t.site.nac);
        CHECK(t.adjacent.size() == 1 && t.adjacent[0].site == 6, "%s: adjacent site", k.name);
        CHECK(t.idens[1].valid && std::fabs(t.idens[1].baseHz - 851006250) < 1 && t.idens[1].spacingHz == 12500 && t.idens[1].txOffsetHz == -45e6, "%s: identifier 1", k.name);
        CHECK(t.idens[2].valid && t.idens[2].vu && t.idens[2].baseHz == 460e6 && t.idens[2].txOffsetHz == 5e6, "%s: identifier 2 (VU)", k.name);
        const P25Call* gr = find(t, 1, 1234567, 1201);
        CHECK(gr && gr->group && gr->channel == 0x1010 && std::fabs(gr->freqHz - 851206250) < 1, "%s: voice grant %.0f", k.name, gr ? gr->freqHz : 0.0);
        bool tg = false;
        for (const auto& g : t.talkgroups) if (g.tg == 1201 && g.grants >= 1) tg = true;
        CHECK(tg, "%s: talkgroup 1201 activity", k.name);
        const P25Call* v = find(t, 0, 1234567, 1201);
        CHECK(v && v->group && !v->encrypted && v->nac == 0x293 && v->clearFrames > 50 && v->lateEntry == k.late && v->endSeen == !k.noTerm,
              "%s: clear call (late %d end %d frames %u)", k.name, v ? v->lateEntry : -1, v ? v->endSeen : -1, v ? v->clearFrames : 0);
        const P25Call* e = find(t, 0, 7654321, 1300);
        CHECK(e && e->encrypted && e->algId == 0x84 && e->keyId == 0x1234 && e->clearFrames == 0 && !e->voice && e->voiceFrames > 50,
              "%s: encrypted call (alg %02X key %d clear %u)", k.name, e ? e->algId : 0, e ? e->keyId : 0, e ? e->clearFrames : 0);
        const P25Call* u = find(t, 0, 2002, 2003);
        CHECK(u && !u->group && u->emergency, "%s: unit to unit emergency call", k.name);
        CHECK(find(t, 2, -1, 2002), "%s: data packet", k.name);
        if (getenv("P25_EV")) for (const auto& ev : t.events) printf("    %s\n", ev.c_str());
        if (k.snr >= 40) CHECK(r.vok >= 160, "%s: voice bits %d", k.name, r.vok);
    }
    {
        P25GenConfig c; c.rate = 500000; c.snrDb = 10; c.loop = false;
        P25Generator g(c);   // no transmissions: noise only
        P25Receiver rx; rx.setSilent(true); rx.configure(c.rate);
        std::vector<cf32> buf(25000);
        for (int i = 0; i < 200; i++) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
        P25Telemetry t; rx.telemetry(t, 0);
        CHECK(t.frames == 0 && t.log.empty() && !t.site.valid && t.events.empty(), "noise: %llu frames, %zu calls", (unsigned long long)t.frames, t.log.size());
    }
    printf(fails ? "p25 rx: %d FAILED\n" : "p25 rx: all passed\n", fails);
    return fails ? 1 : 0;
}
