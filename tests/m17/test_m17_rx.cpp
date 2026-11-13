// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// M17 receiver against the test signal with real-world faults (tests/common/impair.h): tuning error +-1.5 kHz, clock +-100 ppm, deviation
// +-20 %, swapped I/Q, a start in the middle of a stream (the LSF rebuilt from the LICH), and a BER curve over the SNR.
// Each case plays the test schedule once: a voice stream with META text, a packet SMS, a voice + data stream with a GNSS META, and an
// AES-flagged stream.
#include "dect2/m17/m17_gen.h"
#include "dect2/m17/m17_rx.h"
#include "dect2/m17/m17_tel.h"
#include "dect2/m17/m17_codec2.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRate = 500000;
constexpr double kOff = -50000;
constexpr int kStreamFrames = 75 + 60 + 30;

struct Fault { const char* name; double cfo = 0, ppm = 0, dev = 1, snr = 30; bool swap = false; double skip = 0; };
struct Result { M17Telemetry t; double audioRms = 0; size_t audio = 0; };

static Result run(const Fault& f) {
    M17GenConfig c;
    c.rate = kRate; c.dialOffsetHz = kOff; c.snrDb = f.snr; c.cfoHz = f.cfo; c.devScale = f.dev; c.loop = false; c.seed = 3;
    c.txs = m17TestSchedule();
    double total = c.leadSec + 0.5;
    for (const auto& t : c.txs) total += (t.packet ? 0.2 : t.durSec + 0.12) + t.gapSec;
    M17Generator g(c);
    std::vector<cf32> x((size_t)(total * kRate));
    g.generate(x.data(), x.size());
    if (f.ppm != 0) x = impair::clock(x, f.ppm);
    if (f.swap) impair::swapIq(x);
    if (f.skip > 0) impair::skip(x, (size_t)(f.skip * kRate));
    M17Receiver r;
    r.setSilent(true);
    Result res;
    double e = 0;
    r.setAudioTap([&](const float* a, size_t n) { for (size_t i = 0; i < n; i++) e += (double)a[i] * a[i]; res.audio += n; });
    r.configure(kRate);
    r.setSignalOffset(kOff);
    for (size_t o = 0; o < x.size(); o += 8192) r.feed(x.data() + o, std::min<size_t>(8192, x.size() - o));
    r.telemetry(res.t, 0);
    res.audioRms = res.audio ? std::sqrt(e / (double)res.audio) : 0;
    return res;
}

static bool hasMsg(const M17Telemetry& t, const std::string& s) { for (const auto& m : t.messages) if (m.sms && m.text == s) return true; return false; }

int main() {
    const bool c2 = m17Codec2Available();
    printf("  libcodec2: %s\n", c2 ? "found (voice coded and decoded)" : "not found (known frame bits, no audio)");
    const auto sched = m17TestSchedule();
    const Fault cases[] = {
        {"clean"},
        {"offset +1.5 kHz", 1500}, {"offset -1.5 kHz", -1500},
        {"clock +100 ppm", 0, 100}, {"clock -100 ppm", 0, -100},
        {"deviation +20 %", 0, 0, 1.2}, {"deviation -20 %", 0, 0, 0.8},
        {"swapped I/Q", 0, 0, 1, 30, true},
        {"all at once", -1500, 100, 0.8, 20, true},
    };
    printf("  %-18s %5s %7s %5s %6s %5s %4s %6s %s\n", "fault", "LSF", "stream", "SMS", "GNSS", "enc", "inv", "BER%", "first transmission");
    for (const auto& f : cases) {
        const Result r = run(f);
        const M17Telemetry& t = r.t;
        bool gnss = false;
        for (const auto& p : t.positions) gnss |= std::fabs(p.lat - 48.2085) < 0.01 && std::fabs(p.lon - 16.3725) < 0.02 && p.src == "N0CALL/P";
        bool enc = false; std::string first = "-";
        for (const auto& l : t.log) if (l.enc == "AES-256" && l.can == 5 && !l.voice) enc = true;
        if (!t.log.empty()) { const auto& l = t.log.back(); first = l.src + " -> " + l.dst + ", " + l.type + ", CAN " + std::to_string(l.can) + ", \"" + l.text + "\""; }
        printf("  %-18s %5llu %3llu/%3d %5s %6s %5s %4s %6.2f %s\n", f.name, (unsigned long long)t.lsfOk, (unsigned long long)t.streamFrames, kStreamFrames,
               hasMsg(t, sched[1].text) ? "yes" : "no", gnss ? "yes" : "no", enc ? "yes" : "no", t.inverted ? "yes" : "no", t.berPct, first.c_str());
        CHECK(t.lsfOk == 4, "%s: %llu LSF", f.name, (unsigned long long)t.lsfOk);
        CHECK(t.streamFrames >= (uint64_t)kStreamFrames - 2, "%s: %llu stream frames", f.name, (unsigned long long)t.streamFrames);
        CHECK(hasMsg(t, sched[1].text), "%s: SMS", f.name);
        CHECK(gnss, "%s: GNSS position", f.name);
        CHECK(enc, "%s: encrypted stream flagged", f.name);
        CHECK(t.inverted == f.swap, "%s: polarity", f.name);
        CHECK(t.log.size() == 4, "%s: %zu transmissions logged", f.name, t.log.size());
        if (t.log.size() == 4) {
            const auto& v = t.log[3];
            CHECK(v.src == "N0CALL" && v.dst == "@ALL" && v.type == "voice 3200" && v.can == 0 && v.text == sched[0].text && std::fabs(v.durSec - 3.0) < 0.2, "%s: first transmission", f.name);
            CHECK(t.log[1].type == "voice + data 1600" && t.log[1].can == 3 && t.log[1].dst == "ECHO" && t.log[1].data.find("M17 data stream") != std::string::npos, "%s: voice + data stream", f.name);
        }
        if (c2) CHECK(t.voiceFrames >= 2 * 75 + 60 - 6 && r.audioRms > 0.01, "%s: voice %llu frames, rms %.3f", f.name, (unsigned long long)t.voiceFrames, r.audioRms);
        else CHECK(t.voiceFrames == 0 && r.audio == 0, "%s: no voice without codec2", f.name);
    }
    // a start in the middle of the first stream (1.5 s in): the callsigns come from the LICH
    {
        Fault f{"start mid-stream"}; f.skip = 1.5;
        const Result r = run(f);
        const M17Telemetry& t = r.t;
        CHECK(t.log.size() == 4, "mid: %zu transmissions", t.log.size());
        if (!t.log.empty()) {
            const auto& v = t.log.back();
            printf("  %-18s LSF %llu, from LICH %llu: %s -> %s, %s, %u frames, text \"%s\"\n", f.name, (unsigned long long)t.lsfOk, (unsigned long long)t.lichLsf, v.src.c_str(), v.dst.c_str(),
                   v.type.c_str(), v.frames, v.text.c_str());
            CHECK(v.lsfFromLich && v.src == "N0CALL" && v.dst == "@ALL" && v.type == "voice 3200" && v.frames >= 30, "mid: rebuilt LSF");
        }
        CHECK(t.lsfOk == 3 && hasMsg(t, sched[1].text), "mid: the rest of the schedule");
    }
    // BER curve
    printf("  %-8s %8s %8s %6s %5s\n", "C/N 9k", "stream", "BER%", "SMS", "LSF");
    double prevFrames = 1e9;
    for (double snr : {25.0, 18.0, 15.0, 12.0, 10.0, 8.0, 6.0}) {
        Fault f{"snr"}; f.snr = snr;
        const Result r = run(f);
        const M17Telemetry& t = r.t;
        printf("  %5.0f dB %4llu/%3d %8.2f %6s %5llu\n", snr, (unsigned long long)t.streamFrames, kStreamFrames, t.berPct, hasMsg(t, sched[1].text) ? "yes" : "no", (unsigned long long)t.lsfOk);
        if (snr >= 15) CHECK(t.streamFrames >= (uint64_t)kStreamFrames - 2 && t.lsfOk == 4 && hasMsg(t, sched[1].text), "%.0f dB: all decoded", snr);
        if (snr >= 12) CHECK(t.streamFrames >= (uint64_t)kStreamFrames * 9 / 10, "%.0f dB: %llu frames", snr, (unsigned long long)t.streamFrames);
        CHECK((double)t.streamFrames <= prevFrames + 3, "%.0f dB: more frames at lower SNR", snr);
        prevFrames = (double)t.streamFrames;
    }
    // noise only: nothing
    {
        M17Receiver r; r.setSilent(true); r.configure(kRate); r.setSignalOffset(kOff);
        std::vector<cf32> x((size_t)(5 * kRate), cf32(0, 0));
        impair::noise(x, 0, 9);
        for (size_t o = 0; o < x.size(); o += 8192) r.feed(x.data() + o, std::min<size_t>(8192, x.size() - o));
        M17Telemetry t; r.telemetry(t, 0);
        CHECK(t.blocksOk == 0 && t.log.empty(), "noise: %llu frames", (unsigned long long)t.blocksOk);
    }
    printf(fails ? "m17 rx: %d FAILED\n" : "m17 rx: all passed\n", fails);
    return fails ? 1 : 0;
}
