// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sigfox mode on its test signal: every made-up device must arrive with its ID, payload (or single bit), downlink flag and all three ranks
// merged into one message: clean, at 14 dB in 100 Hz, with a +-1.5 kHz radio offset, with the carrier drifting 15 Hz/s during each frame
// (Doppler and oscillator drift), at 600 bit/s, and at another sample rate. Noise alone must give nothing.
#include "dect2/sigfox/sigfox_gen.h"
#include "dect2/sigfox/sigfox_proto.h"
#include "dect2/sigfox/sigfox_rx.h"
#include "dect2/sigfox/sigfox_tel.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static SigfoxModeTelemetry run(const SynthConfig& cfg, double rate, double seconds, bool noiseOnly = false) {
    SigfoxReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-sigfoxTuning().tuneOffsetHz);
    rx.setDialHz(868.130e6);
    auto syn = makeSigfoxSynth(cfg, rate);
    std::vector<cf32> buf(65536);
    SigfoxModeTelemetry t;
    uint64_t seq = 0;
    unsigned s = 4242;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        if (noiseOnly) {
            for (auto& v : buf) { s = s * 1664525u + 1013904223u; const float a = ((s >> 8) & 0xFFFF) / 65536.f - 0.5f; s = s * 1664525u + 1013904223u; v = cf32(a * 0.3f, (((s >> 8) & 0xFFFF) / 65536.f - 0.5f) * 0.3f); }
        } else syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        SigfoxModeTelemetry n;
        if (rx.telemetry(n, seq)) { seq = n.seq; t = n; }
    }
    return t;
}

static void expectAll(const SigfoxModeTelemetry& t, const char* what, int wantRanks) {
    int got = 0;
    for (const sigfox::TestDevice& d : sigfox::testDevices()) {
        const std::string id = sigfox::idHex(d.id);
        const SigfoxMessage* m = nullptr;
        for (const SigfoxMessage& q : t.sf.messages) if (q.id == id && (!m || q.reps > m->reps)) m = &q;   // the next cycle may have begun
        CHECK(m != nullptr, "%s: device %s missing", what, id.c_str());
        if (!m) continue;
        CHECK(m->payload == sigfox::hexOf(d.payload), "%s: %s payload %s", what, id.c_str(), m->payload.c_str());
        CHECK(m->singleBit == d.singleBit && (!d.singleBit || m->bitValue == d.bitValue), "%s: %s single bit", what, id.c_str());
        CHECK(m->downlinkRequest == d.downlinkRequest, "%s: %s downlink flag", what, id.c_str());
        CHECK(m->rankMask == wantRanks, "%s: %s ranks %d (want %d), %d reps", what, id.c_str(), m->rankMask, wantRanks, m->reps);
        got++;
    }
    for (const SigfoxMessage& q : t.sf.messages) {
        bool known = false;
        for (const sigfox::TestDevice& d : sigfox::testDevices()) known = known || q.id == sigfox::idHex(d.id);
        CHECK(known, "%s: stray message %s", what, q.id.c_str());
    }
    printf("  %-28s %d/4 devices, %zu messages, %llu carriers, %llu frames\n", what, got, t.sf.messages.size(), (unsigned long long)t.sf.carriers, (unsigned long long)t.sf.frames);
}

int main() {
    const double rate = 1024000;
    const double cycle100 = 33;   // four messages of three frames each at 100 bit/s, with the gaps
    { SynthConfig c; c.snrDb = 30; expectAll(run(c, rate, cycle100), "clean", 7); }
    { SynthConfig c; c.snrDb = 14; c.modeOpt[3] = 5; expectAll(run(c, rate, cycle100), "14 dB", 7); }
    { SynthConfig c; c.snrDb = 20; c.cfoHz = 1500; expectAll(run(c, rate, cycle100), "+1.5 kHz", 7); }
    { SynthConfig c; c.snrDb = 20; c.cfoHz = -1500; c.modeVal[0] = 15; expectAll(run(c, rate, cycle100), "-1.5 kHz, drift 15 Hz/s", 7); }
    { SynthConfig c; c.snrDb = 25; c.modeOpt[0] = 1; expectAll(run(c, rate, 15), "600 bit/s", 7); }
    { SynthConfig c; c.snrDb = 25; c.modeOpt[1] = 1; expectAll(run(c, 2048000, 15), "one frame, 2.048 Msps", 1); }
    {
        SynthConfig c;
        const SigfoxModeTelemetry t = run(c, rate, 10, true);
        CHECK(t.sf.messages.empty(), "noise: %zu messages", t.sf.messages.size());
        CHECK(t.state == 1, "noise: state %d", t.state);
        CHECK(t.zone == "RC1", "zone '%s'", t.zone.c_str());
    }
    return testSummary("sigfox rx");
}
