// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL in the engine's wrapper and the transmit session's generator: the transmit generator (the TX path, 48 kHz) makes the signal from speech pulled as
// 48 kHz stereo, KaalLiveReceiver (what the engine feeds) locks, decodes the speech to its tap and publishes the beacon in the telemetry.
#include "dect2/kaal/kaal_live.h"
#include "dect2/kaal/kaal_tel.h"
#include "dect2/kaal/kaal_txgen.h"
#include "dect2/common/ring.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const double rate = 48000;
    uint64_t pulled = 0;
    TxRequest r;
    r.modeId = "kaal";
    r.callsign = "ly2abc";   // raised to upper case
    r.grid = "KO26ab";
    r.lat = 54.687; r.lon = 25.279;
    r.text = "Hi, I am testing KAAL.";
    r.opt["kaal.powerDbm"] = 20;
    r.opt["kaal.beacon"] = 1;
    r.pullAudio = [&](float* st, int n) {   // speech-like: a wobbling pitch with a slow level change
        for (int i = 0; i < n; i++, pulled++) {
            const double t = (double)pulled / 48000.0;
            const float v = (float)(0.09 * std::sin(2 * 3.141592653589793 * (150 + 60 * std::sin(2 * 3.141592653589793 * 1.3 * t)) * t) * (0.6 + 0.4 * std::sin(2 * 3.141592653589793 * 3 * t)));
            st[2 * i] = st[2 * i + 1] = v;
        }
        return n;
    };
    std::string err;
    auto gen = makeKaalTxGenerator(r, rate, err);
    CHECK(gen != nullptr, "generator: %s", err.c_str());
    if (!gen) return testSummary("kaal_engine");
    CHECK(std::fabs(gen->sampleRate() - rate) < 1, "rate %.0f", gen->sampleRate());

    KaalLiveReceiver rx;
    rx.setSilent(true);
    size_t tapSamples = 0;
    rx.setAudioTap([&](const int16_t*, size_t n) { tapSamples += n; });
    rx.configure(rate);
    CHECK(rx.ready(), "ready at 48 kHz");

    KaalTelemetry last;
    uint64_t seq = 0;
    std::vector<cf32> blk(4800);
    for (int i = 0; i < 300; i++) {   // 30 s (the sweep ID needs two cycles of 12 s)
        gen->generate(blk.data(), blk.size());
        rx.feed(blk.data(), blk.size());
        KaalTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; last = t; if (t.beaconValid) { /* keep the last with a beacon */ } }
    }
    printf("state %d, frames good %llu bad %llu, snr %.1f dB, tap %zu samples, beacons %llu, callsign '%s' grid '%s'\n", last.state, (unsigned long long)last.blocksOk,
           (unsigned long long)last.blocksBad, (double)last.snrDb, tapSamples, (unsigned long long)last.beacons, last.callsign.c_str(), last.grid.c_str());
    CHECK(pulled > 0, "the generator pulled audio");
    CHECK(last.state == 2, "locked (state %d)", last.state);
    CHECK(last.blocksOk >= 100, "good frames %llu", (unsigned long long)last.blocksOk);
    CHECK(last.bitrateKbps == 16, "the default voice bitrate is 16 kbit/s, telemetry says %d", last.bitrateKbps);
    CHECK(last.sweepIdValid && last.sweepId == "LY2ABC", "sweep ID '%s' (valid %d)", last.sweepId.c_str(), (int)last.sweepIdValid);
    CHECK(last.blocksBad <= last.blocksOk / 10, "bad frames %llu", (unsigned long long)last.blocksBad);
    CHECK(tapSamples >= 16000 * 20, "decoded speech %zu samples", tapSamples);
    CHECK(last.beaconValid && last.callsign == "LY2ABC" && last.grid == "KO26ab", "beacon '%s' '%s'", last.callsign.c_str(), last.grid.c_str());
    CHECK(std::fabs(last.lat - 54.687) < 0.01 && std::fabs(last.lon - 25.279) < 0.01 && std::fabs(last.powerDbm - 20) < 0.6, "beacon position %.3f %.3f power %.1f", last.lat, last.lon, last.powerDbm);

    // every codec bitrate is accepted, anything else refused; a callsign too long for the sweep ID is only sent in the beacon
    for (int kb : {12, 16, 24, 32}) { TxRequest q = r; q.opt["kaal.bitrate"] = kb; CHECK(makeKaalTxGenerator(q, rate, err) != nullptr, "bitrate %d refused: %s", kb, err.c_str()); }
    { TxRequest q = r; q.opt["kaal.bitrate"] = 20; CHECK(!makeKaalTxGenerator(q, rate, err) && !err.empty(), "bitrate 20 must be refused"); }
    { TxRequest q = r; q.callsign = "OH2XYZ123"; CHECK(makeKaalTxGenerator(q, rate, err) != nullptr, "a 9-character callsign is fine for the beacon: %s", err.c_str()); }
    // an unusable callsign is refused in plain words, and the sound-card path of a rate below 48 kHz too
    TxRequest bad = r;
    bad.callsign = "WAYTOOLONGCALL";
    CHECK(!makeKaalTxGenerator(bad, rate, err) && !err.empty(), "a callsign of more than 9 characters is refused");
    CHECK(!makeKaalTxGenerator(r, 24000, err), "a rate below 48 kHz is refused");
    return testSummary("kaal_engine");
}
