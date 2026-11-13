// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA Direct Mode: the PDUs, the colour code, and the receiver against the generator, with frequency offsets, a clock offset, noise and an encrypted call. Generated signals only: no real
// DMO radio was heard.
#include "dect2/tetra/tetra_voice.h"
#include "dect2/tetradmo/tetradmo_gen.h"
#include "dect2/tetradmo/tetradmo_rx.h"
#include "dect2/tetradmo/tetradmo_tel.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::tetradmo;
using namespace dect2::tetra;
#include "../common/test_util.h"

struct Sink final : tetra::SpeechFrameSink {
    std::vector<Bits> frames;
    void onCodedFrame(const uint8_t* b, size_t n) override { frames.push_back(Bits(b, b + n)); }
};

static void testPdus() {
    SyncS s; s.commType = 1; s.msFlag = true; s.slot = 3; s.frame = 17; s.aiEncState = 2; s.tvp = 0x1F00BA; s.ksg = 5; s.keyNumber = 9; s.abUsage = 1;
    SyncS o;
    CHECK(parseSyncS(encodeSyncS(s), o) && o.commType == 1 && o.msFlag && o.slot == 3 && o.frame == 17 && o.aiEncState == 2 && o.tvp == 0x1F00BA && o.ksg == 5 && o.keyNumber == 9, "SCH/S round trip");
    CHECK(encodeSyncS(s).size() == 60, "SCH/S is 60 bits");
    DmMessage m; m.dest = 0xABCDEF; m.src = 0x123456; m.mni = (262u << 14) | 42; m.messageType = (int)Dm::Setup; m.setupKnown = true; m.groupCall = false; m.e2eEncrypted = true; m.circuitMode = 2; m.priority = 3; m.powerClass = 2; m.frameCountdown = 2;
    Bits h = encodeSyncH(m, 0);
    CHECK(h.size() == 124, "SCH/H is 124 bits, %zu", h.size());
    DmMessage r;
    CHECK(parseSyncH(h, 0, r) && r.dest == m.dest && r.src == m.src && r.mni == m.mni && r.messageType == (int)Dm::Setup && r.setupKnown && !r.groupCall && r.e2eEncrypted && r.circuitMode == 2 && r.priority == 3 && r.frameCountdown == 2, "SCH/H round trip");
    CHECK(dccInit(m.mni, m.src) == (((m.mni & 0x3F) << 26 | (0x123456u << 2)) | 3u), "colour code bits");
    // the channel coding through the shared TMO routines
    Bits t(60, 0); t[5] = 1; t[40] = 1;
    Bits code = encodeChannel(t, kBsch, zeroInit());
    std::vector<float> llr(code.size());
    for (size_t i = 0; i < code.size(); i++) llr[i] = code[i] ? 2.f : -2.f;
    Bits back;
    CHECK(decodeChannel(llr.data(), kBsch, zeroInit(), back) && back == t, "SCH/S channel coding");
    CHECK(!decodeChannel(llr.data(), kBsch, 0xDEADBEEF, back), "wrong colour code fails the CRC");
    // burst layout: training at bit 230 (1-based 231 to 252), phase rule
    Bits bk(216, 0);
    Bits b = dnb(bk, bk, false);
    CHECK(b.size() == kBurstBits, "DNB length");
    for (int i = 0; i < 22; i++) CHECK(b[(size_t)(kTrainAt + i)] == kTrainN[i], "DNB training bit %d", i);
    Bits ds = dsb(Bits(120, 0), bk);
    for (int i = 0; i < 38; i++) CHECK(ds[(size_t)(kYAt + i)] == kTrainY[i], "DSB training bit %d", i);
    // the phase adjustment rule: the sum of phase steps over symbols 8..126 is a multiple of 2 pi
    int sum = 0;
    for (int n = 7; n <= 126; n++) sum += dibitPhase(b[(size_t)(2 * n - 2)], b[(size_t)(2 * n - 1)]);
    CHECK(((sum % 8) + 8) % 8 == 0, "phase adjustment: residue %d", ((sum % 8) + 8) % 8);
}

struct Res { TetraDmoTelemetry t; std::vector<Bits> frames; };
static Res run(const TetraDmoGenConfig& c, double offset = 0) {
    TetraDmoGenConfig cc = c;
    cc.cfoHz += offset;
    TetraDmoGenOut gen = tetraDmoSignal(cc);
    TetraDmoReceiver rx;
    rx.configure(cc.rate);
    rx.setSignalOffset(offset);
    Sink sink;
    rx.setVoiceSink(&sink);
    for (size_t i = 0; i < gen.iq.size(); i += 1024) rx.feed(gen.iq.data() + i, std::min<size_t>(1024, gen.iq.size() - i));
    std::vector<cf32> tail((size_t)(cc.rate * 1.0));
    for (size_t i = 0; i < tail.size(); i += 1024) rx.feed(tail.data() + i, std::min<size_t>(1024, tail.size() - i));
    Res r;
    rx.telemetry(r.t, 0);
    r.frames = sink.frames;
    return r;
}

static void expectClear(const char* name, const TetraDmoGenConfig& c, double offset = 0) {
    Res r = run(c, offset);
    TetraDmoGenConfig cc = c; cc.cfoHz += offset;
    TetraDmoGenOut gen = tetraDmoSignal(cc);
    CHECK(r.t.dsb >= 4, "%s: only %llu DSB", name, (unsigned long long)r.t.dsb);
    CHECK(r.t.lastSrc == c.src || r.t.lastDest == c.dest, "%s: addresses", name);
    bool foundCall = false;
    for (const auto& call : r.t.calls) if (call.src == c.src && call.dest == c.dest && call.mni == c.mni) foundCall = true;
    CHECK(foundCall, "%s: no call for the addresses", name);
    int good = 0;
    for (size_t i = 0; i < r.frames.size(); i++) {
        for (const auto& tx : gen.trafficBits) {
            if (r.frames[i].size() != tx.size()) continue;
            int bad = 0;
            for (size_t k = 0; k < tx.size(); k++) bad += r.frames[i][k] != tx[k];
            if (bad <= 8) { good++; break; }
        }
    }
    printf("%-20s: DSB %llu, DNB %llu, traffic %zu of %d delivered, %d intact\n", name, (unsigned long long)r.t.dsb, (unsigned long long)r.t.dnb, r.frames.size(), c.trafficFrames, good);
    CHECK((int)r.frames.size() >= c.trafficFrames * 8 / 10, "%s: delivered %zu of %d", name, r.frames.size(), c.trafficFrames);
    CHECK(good >= (int)r.frames.size() * 8 / 10, "%s: only %d intact of %zu", name, good, r.frames.size());
}

int main() {
    testPdus();
    TetraDmoGenConfig c;
    expectClear("clean", c);
    { auto x = c; x.rate = 240000; expectClear("240 ksps", x); }
    { auto x = c; x.rate = 2400000; expectClear("2.4 Msps", x, 40000); }
    { auto x = c; x.cfoHz = 800; expectClear("cfo +800", x); }
    { auto x = c; x.cfoHz = -1800; expectClear("cfo -1800", x); }
    { auto x = c; x.sroPpm = 100; expectClear("sro +100 ppm", x); }
    { auto x = c; x.noise = 0.02; expectClear("noise 0.02", x); }
    { auto x = c; x.noise = 0.05; expectClear("noise 0.05", x); }
    { auto x = c; x.commType = 1; expectClear("via repeater", x); }
    { auto x = c; x.groupCall = false; x.dest = 0x000042; expectClear("individual", x); }
    {   // encrypted: reported, and no traffic reaches the sink
        auto x = c; x.aiEncState = 1;
        Res r = run(x);
        CHECK(r.t.dsb >= 4 && r.frames.empty() && r.t.encState == 1, "air interface encrypted: frames %zu state %d", r.frames.size(), r.t.encState);
        CHECK(r.t.trafficRefused > 0, "refused bursts counted");
        bool enc = false;
        for (const auto& call : r.t.calls) enc |= call.encrypted;
        CHECK(enc, "call marked encrypted");
    }
    {
        auto x = c; x.e2eEncrypted = true;
        Res r = run(x);
        CHECK(r.frames.empty(), "end-to-end encrypted: frames %zu", r.frames.size());
    }
    {   // noise only
        TetraDmoReceiver rx; rx.configure(72000);
        std::mt19937 rng(9); std::normal_distribution<float> g(0.f, 0.05f);
        std::vector<cf32> iq(72000 * 4);
        for (auto& v : iq) v = cf32(g(rng), g(rng));
        for (size_t i = 0; i < iq.size(); i += 1024) rx.feed(iq.data() + i, std::min<size_t>(1024, iq.size() - i));
        TetraDmoTelemetry t;
        rx.telemetry(t, 0);
        CHECK(t.dsb == 0, "noise found %llu DSB", (unsigned long long)t.dsb);
    }
    return testSummary("tetradmo_rx");
}
