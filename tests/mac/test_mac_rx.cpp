// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MAC receiver against the test signal: D2-MAC and D-MAC at 20 Msps (the outer part of the 27 MHz channel cut off as a 20 Msps radio
// does), lock, the picture (colour bars against the test card), the channel name, the tone of the sound, +-200 kHz offset, 18 dB C/N,
// 16:9, EuroCrypt scrambling, spectrum inversion, noise only. Silent.
#include "dect2/mac/mac_gen.h"
#include "dect2/mac/mac_rx.h"
#include "dect2/mac/mac_tel.h"
#include "dect2/atv/atv_card.h"
#include "dect2/atv/atv_std.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Result {
    MacTelemetry t;
    std::shared_ptr<const AtvFrame> frame;
    std::vector<float> audio;
    double secs = 0;
};

static Result run(const MacGenConfig& c, double seconds, double rxRate = 0) {
    MacGenerator g(c);
    MacReceiver rx;
    rx.setSilent(true);
    Result r;
    rx.setAudioTap([&](const float* x, size_t n) { r.audio.insert(r.audio.end(), x, x + n); });
    rx.configure(rxRate > 0 ? rxRate : c.rate);
    std::vector<cf32> buf(1 << 16);
    const size_t total = (size_t)(seconds * c.rate);
    uint64_t fseq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t done = 0; done < total; done += buf.size()) {
        g.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        if (auto f = rx.frame(fseq)) r.frame = f;
    }
    r.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    rx.telemetry(r.t, 0);
    return r;
}

// tone frequency from the zero crossings of the last part of the sound
static double toneHz(const std::vector<float>& a) {
    if (a.size() < 8000) return 0;
    const size_t from = a.size() / 2;
    int zc = 0; size_t first = 0, last = 0;
    for (size_t i = from + 1; i < a.size(); i++)
        if (a[i - 1] < 0 && a[i] >= 0) { if (!zc) first = i; last = i; zc++; }
    return zc > 2 ? (zc - 1) * 32000.0 / (double)(last - first) : 0;
}

// the colour bars of the decoded picture against the test card: mean absolute error (0..255) over the middle of each bar
static double barsError(const AtvFrame& f) {
    AtvFormat fm; atvMakeFormat(kAtvG, kAtvPal, fm);
    AtvCard card(fm, 0);
    std::vector<uint8_t> ref;
    card.referenceFrame(0, 0, ref);
    int x0, x1, y0, y1;
    card.barsArea(x0, x1, y0, y1);
    const double sx = f.width / 768.0;
    double e = 0; int n = 0;
    const int bw = (x1 - x0) / 8;
    for (int b = 0; b < 8; b++)
        for (int y = y0 + (y1 - y0) / 4; y < y1 - (y1 - y0) / 4; y++)
            for (int x = x0 + b * bw + bw / 4; x < x0 + (b + 1) * bw - bw / 4; x++) {
                const int xd = (int)std::lround((x + 0.5) * sx - 0.5);
                for (int c = 0; c < 3; c++) { e += std::fabs((double)f.rgba[((size_t)y * f.width + xd) * 4 + c] - ref[((size_t)y * 768 + x) * 4 + c]); n++; }
            }
    return n ? e / n : 999;
}

static void report(const char* label, const Result& r) {
    printf("  %-22s %s | tone %.1f Hz | bars error %.1f | %.1f s to decode\n", label, macSummary(r.t).c_str(), toneHz(r.audio), r.frame ? barsError(*r.frame) : -1.0, r.secs);
}

int main() {
    // ---- D2-MAC, clean
    {
        MacGenConfig c; c.snrDb = 40; c.toneHz = 1000;
        Result r = run(c, 1.0);
        report("D2-MAC 40 dB", r);
        CHECK(r.t.state == 2 && r.t.variant == 0, "state %d variant %d", r.t.state, r.t.variant);
        CHECK(r.t.channelName == "ONAIR TV", "name '%s'", r.t.channelName.c_str());
        CHECK(r.t.networkName == "ONAIR SATELLITE" && r.t.country == "LITHUANIA", "network '%s' country '%s'", r.t.networkName.c_str(), r.t.country.c_str());
        CHECK(!r.t.scrambled && r.t.aspectKnown && !r.t.wide, "flags scrambled %d wide %d", r.t.scrambled, r.t.wide);
        CHECK(std::fabs(toneHz(r.audio) - 1000) < 5, "tone %.1f Hz", toneHz(r.audio));
        CHECK(r.frame && r.frame->width == 768 && barsError(*r.frame) < 14, "picture: bars error %.1f", r.frame ? barsError(*r.frame) : -1.0);
        CHECK(r.t.soundErrors == 0 && r.t.packetsCorrected == 0, "sound errors %llu, corrected headers %llu", (unsigned long long)r.t.soundErrors, (unsigned long long)r.t.packetsCorrected);
        CHECK(r.t.chid == 0x1A2B, "channel id %04X", r.t.chid);
    }
    // ---- carrier offset +-200 kHz, another tone
    for (double cfo : {200e3, -200e3}) {
        MacGenConfig c; c.snrDb = 30; c.cfoHz = cfo; c.toneHz = 1500; c.seed = 3;
        Result r = run(c, 1.0);
        report(cfo > 0 ? "D2-MAC +200 kHz" : "D2-MAC -200 kHz", r);
        CHECK(r.t.state == 2 && r.t.channelName == "ONAIR TV", "%+.0f kHz: state %d name '%s'", cfo / 1e3, r.t.state, r.t.channelName.c_str());
        CHECK(std::fabs(r.t.cfoHz - cfo) < 20e3, "offset measured %.1f kHz", r.t.cfoHz / 1e3);
        CHECK(std::fabs(toneHz(r.audio) - 1500) < 8, "tone %.1f Hz", toneHz(r.audio));
        CHECK(r.frame && barsError(*r.frame) < 16, "picture: bars error %.1f", r.frame ? barsError(*r.frame) : -1.0);
    }
    // ---- 18 dB C/N (in 27 MHz)
    {
        MacGenConfig c; c.snrDb = 18; c.seed = 5;
        Result r = run(c, 1.5);
        report("D2-MAC 18 dB", r);
        CHECK(r.t.state == 2 && r.t.channelName == "ONAIR TV", "18 dB: state %d name '%s'", r.t.state, r.t.channelName.c_str());
        CHECK(std::fabs(toneHz(r.audio) - 1000) < 8, "18 dB: tone %.1f Hz", toneHz(r.audio));
        CHECK(r.frame && barsError(*r.frame) < 22, "18 dB: bars error %.1f", r.frame ? barsError(*r.frame) : -1.0);
    }
    // ---- 16:9
    {
        MacGenConfig c; c.wide = true; c.seed = 6;
        Result r = run(c, 0.8);
        report("D2-MAC 16:9", r);
        CHECK(r.t.aspectKnown && r.t.wide && r.frame && r.frame->width == 1024, "16:9 flag %d, width %d", r.t.wide, r.frame ? r.frame->width : 0);
        CHECK(r.frame && barsError(*r.frame) < 16, "16:9: bars error %.1f", r.frame ? barsError(*r.frame) : -1.0);
    }
    // ---- EuroCrypt: detected, not descrambled
    {
        MacGenConfig c; c.scrambled = true; c.seed = 7;
        Result r = run(c, 0.8);
        report("D2-MAC EuroCrypt", r);
        CHECK(r.t.state == 2 && r.t.scrambled && r.t.caSystem == "EuroCrypt", "scrambled %d ca '%s'", r.t.scrambled, r.t.caSystem.c_str());
        CHECK(r.t.controlledAccess && r.t.soundScrambled && r.t.ecmPackets > 0 && r.audio.empty(), "controlled %d sound scrambled %d ECM %llu audio %zu", r.t.controlledAccess,
              r.t.soundScrambled, (unsigned long long)r.t.ecmPackets, r.audio.size());
        CHECK(r.t.channelName == "ONAIR TV", "the name is clear: '%s'", r.t.channelName.c_str());
    }
    // ---- spectrum inverted (I and Q swapped)
    {
        MacGenConfig c; c.invert = true; c.seed = 8;
        Result r = run(c, 1.0);
        report("D2-MAC inverted", r);
        CHECK(r.t.state == 2 && r.t.inverted && r.frame && barsError(*r.frame) < 16, "inverted: state %d inv %d bars %.1f", r.t.state, r.t.inverted, r.frame ? barsError(*r.frame) : -1.0);
    }
    // ---- D-MAC
    {
        MacGenConfig c; c.variant = 1; c.snrDb = 35; c.toneHz = 800; c.seed = 9;
        Result r = run(c, 1.0);
        report("D-MAC 35 dB", r);
        CHECK(r.t.state == 2 && r.t.variant == 1, "D-MAC: state %d variant %d", r.t.state, r.t.variant);
        CHECK(r.t.channelName == "ONAIR TV", "D-MAC: name '%s'", r.t.channelName.c_str());
        CHECK(std::fabs(toneHz(r.audio) - 800) < 5, "D-MAC: tone %.1f Hz", toneHz(r.audio));
        CHECK(r.frame && barsError(*r.frame) < 16, "D-MAC: bars error %.1f", r.frame ? barsError(*r.frame) : -1.0);
    }
    // ---- D-MAC, +200 kHz, 20 dB
    {
        MacGenConfig c; c.variant = 1; c.snrDb = 20; c.cfoHz = 200e3; c.seed = 10;
        Result r = run(c, 1.2);
        report("D-MAC 20 dB +200 kHz", r);
        CHECK(r.t.state == 2 && r.t.channelName == "ONAIR TV" && std::fabs(toneHz(r.audio) - 1000) < 8, "D-MAC 20 dB: state %d name '%s' tone %.1f", r.t.state, r.t.channelName.c_str(), toneHz(r.audio));
    }
    // ---- the radio's clock off by +-60 ppm, other sample rates
    for (double ppm : {60.0, -60.0}) {
        MacGenConfig c; c.rate = 20e6 * (1 + ppm * 1e-6); c.seed = 11;
        Result r = run(c, 1.0, 20e6);
        char lb[40]; snprintf(lb, sizeof lb, "D2-MAC clock %+.0f ppm", ppm);
        report(lb, r);
        CHECK(r.t.state == 2 && r.t.channelName == "ONAIR TV" && std::fabs(r.t.lineErrPpm + ppm) < 10, "%+.0f ppm: state %d, line rate %+.1f ppm", ppm, r.t.state, r.t.lineErrPpm);
        CHECK(r.frame && barsError(*r.frame) < 16 && std::fabs(toneHz(r.audio) - 1000 * (1 + ppm * 1e-6)) < 8, "%+.0f ppm: bars %.1f tone %.1f", ppm, r.frame ? barsError(*r.frame) : -1.0, toneHz(r.audio));
    }
    for (double fs : {16e6, 25e6}) {
        for (int v : {0, 1}) {
            MacGenConfig c; c.rate = fs; c.variant = v; c.snrDb = 30; c.seed = 12;
            Result r = run(c, 1.0);
            char lb[40]; snprintf(lb, sizeof lb, "%s %.0f Msps", v ? "D-MAC" : "D2-MAC", fs / 1e6);
            report(lb, r);
            const bool needOk = !(v == 1 && fs < 20e6);   // D-MAC's 20.25 Mbit/s burst does not fit 16 Msps
            if (needOk) CHECK(r.t.state == 2 && r.t.channelName == "ONAIR TV" && r.frame && barsError(*r.frame) < 18, "%s: state %d name '%s' bars %.1f", lb, r.t.state,
                              r.t.channelName.c_str(), r.frame ? barsError(*r.frame) : -1.0);
        }
    }
    // ---- noise only: no lock
    {
        MacGenConfig c; c.snrDb = -20;
        Result r = run(c, 0.5);
        CHECK(r.t.state == 0, "noise only: state %d", r.t.state);
    }
    printf(fails ? "mac rx: %d FAILED\n" : "mac rx: all passed\n", fails);
    return fails ? 1 : 0;
}
