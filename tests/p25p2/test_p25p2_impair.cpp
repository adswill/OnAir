// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// P25 Phase 2 (H-DQPSK TDMA voice channel, scrambled) under real-world impairments: carrier offsets (also with the channel off centre), a noise
// sweep, a simulcast echo, Rayleigh fading, a sample drop-out and a symbol clock error. Two calls (one clear on slot 1, one encrypted on slot 2) run
// through the receiver; measured per scenario: time to the first sync, the share of the clear call's voice frames that arrive intact, ESS success,
// and false decodes (a frame of the encrypted call handed on as clear, a call nobody sent). Silent. P25P2_IMPAIR_TABLE=1 prints every scenario.
#include "dect2/p25p2/p25p2_gen.h"
#include "dect2/p25p2/p25p2_proto.h"
#include "dect2/p25p2/p25p2_rx.h"
#include "dect2/p25p2/p25p2_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

namespace {

struct Scn {
    const char* name;
    double snr = 30, cfo = 0, rate = 240000;
    double echoUs = 0, echoDb = -6, echoDf = 0;
    double fadeHz = 0;
    double dropSec = 0;
    double ppm = 0;
    bool harsh = false, noiseOnly = false;
};

struct Result {
    double lockSec = -1, voicePct = 0, essPct = 0;
    int falseClear = 0, falseCalls = 0;
    bool callsOk = false;
    unsigned long long syncs = 0, unlocks = 0, realigns = 0;
    double cfo = 0;
};

Result run(const Scn& s) {
    P25p2GenConfig c;
    c.rate = s.rate; c.snrDb = 80; c.superframes = 40; c.scramble = true; c.cfoHz = s.cfo;
    P25p2GenCall a; a.channel = 1; a.startSf = 1; a.nSf = 34; a.src = 1234567; a.group = 1001;
    P25p2GenCall b; b.channel = 2; b.startSf = 2; b.nSf = 33; b.src = 777; b.group = 2002; b.algId = 0x84;
    c.calls = {a, b};
    c.sysMsgs = {p25p2::msgGrpVGrant(0x00, 0x1005, 3003, 55555)};
    std::vector<cf32> x = p25p2Signal(c);
    if (s.ppm != 0) {   // symbol clock error of the transmitter: resample linearly
        const double step = 1.0 + s.ppm * 1e-6;
        std::vector<cf32> y;
        y.reserve(x.size());
        for (double p = 0; p + 1 < (double)x.size(); p += step) { const size_t k = (size_t)p; const float f = (float)(p - k); y.push_back(x[k] * (1 - f) + x[k + 1] * f); }
        x.swap(y);
    }
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.f, 1.f);
    std::uniform_real_distribution<double> ud(0, 2 * M_PI);
    double pw = 0;
    for (const cf32& v : x) pw += std::norm(v);
    pw /= (double)x.size();
    // noise in the 6 kHz of the symbol rate
    const double sigma = std::sqrt(pw / std::pow(10.0, s.snr / 10) * s.rate / 6000.0 / 2);
    double fph[8], fdop[8];
    for (int i = 0; i < 8; i++) { fph[i] = ud(rng); fdop[i] = s.fadeHz * std::cos(2 * M_PI * (i + 0.5) / 16 + 0.3); }
    const size_t dly = (size_t)std::llround(s.echoUs * 1e-6 * s.rate);
    const double echoG = std::pow(10.0, s.echoDb / 20);
    std::vector<cf32> o(x.size());
    for (size_t i = 0; i < x.size(); i++) {
        const double ts = (double)i / s.rate;
        cf32 v = s.noiseOnly ? cf32(0, 0) : x[i];
        if (dly && i >= dly) v += (float)echoG * x[i - dly] * cf32((float)std::cos(2 * M_PI * s.echoDf * ts), (float)std::sin(2 * M_PI * s.echoDf * ts));
        if (s.fadeHz > 0) {
            double re = 0, im = 0;
            for (int k = 0; k < 8; k++) { const double ar = 2 * M_PI * fdop[k] * ts + fph[k]; re += std::cos(ar); im += std::sin(ar); }
            v *= cf32((float)(re / 4.0), (float)(im / 4.0));   // eight unit phasors: power 8, scaled to 1/2 ... then below to 1
            v *= 1.4142136f;
        }
        if (s.dropSec > 0 && std::fmod(ts, 2.0) > 1.5 && std::fmod(ts, 2.0) < 1.5 + s.dropSec) v = cf32(0, 0);
        v += cf32(sigma * nd(rng), sigma * nd(rng));
        o[i] = v;
    }
    P25p2Receiver rx;
    rx.setSilent(true);
    rx.configure(s.rate);
    rx.setSystem(c.wacn, c.sysId, c.colorCode);
    int clearB = 0, matched = 0, next = 0;   // next: the pattern index expected next (frames that never arrive skip ahead)
    rx.setVoiceTap([&](int slot, const uint8_t* bits, bool clear) {
        if (slot == 1 && clear) {
            for (int k = std::max(0, next - 2); k < next + 60; k++) {
                uint8_t ex[72];
                p25p2VoiceBits(1, k, ex);
                if (!std::memcmp(ex, bits, 72)) { matched++; next = k + 1; break; }
            }
        } else if (slot == 2 && clear) clearB++;
    });
    Result r;
    size_t fed = 0;
    for (size_t i = 0; i < o.size(); i += 12000) {
        const size_t n = std::min<size_t>(12000, o.size() - i);
        rx.feed(o.data() + i, n);
        fed += n;
        P25p2Telemetry t;
        if (rx.telemetry(t, 0) && r.lockSec < 0 && t.syncs > 0) { r.lockSec = (double)fed / s.rate; }
    }
    P25p2Telemetry t;
    rx.telemetry(t, 0);
    const int want1 = p25p2VoiceFrames(c, 1);
    r.voicePct = 100.0 * matched / std::max(1, want1);
    r.syncs = t.syncs; r.unlocks = t.unlocks; r.realigns = t.realigns; r.cfo = t.cfoHz;
    r.essPct = 100.0 * t.essOk / std::max<uint64_t>(1, t.essOk + t.essBad);
    r.falseClear = clearB;
    bool a1 = false, b1 = false;
    for (const auto& cl : t.log) {
        if (cl.kind != 0) continue;
        if (cl.slot == 1 && cl.src == 1234567) a1 = true;
        else if (cl.slot == 2 && cl.src == 777) b1 = true;
        else if (cl.src >= 0 && cl.src != 1234567 && cl.src != 777) r.falseCalls++;
    }
    r.callsOk = a1 && b1;
    return r;
}

} // namespace

int main() {
    const bool table = getenv("P25P2_IMPAIR_TABLE") != nullptr;
    std::vector<Scn> scn;
    auto S = [&](Scn s) { scn.push_back(s); };
    { Scn s{"clean"}; S(s); }
    { Scn s{"cfo +1 kHz"}; s.cfo = 1000; S(s); }
    { Scn s{"cfo -2 kHz"}; s.cfo = -2000; S(s); }
    { Scn s{"cfo +2 kHz, 960 ksps"}; s.cfo = 2000; s.rate = 960000; S(s); }
    { Scn s{"cfo +2.6 kHz"}; s.cfo = 2600; S(s); }
    { Scn s{"cfo -2.6 kHz, snr 16 dB"}; s.cfo = -2600; s.snr = 16; S(s); }
    { Scn s{"noise only, 14 s"}; s.noiseOnly = true; s.snr = 0; s.harsh = true; S(s); }
    { Scn s{"cfo +4 kHz"}; s.cfo = 4000; S(s); }
    { Scn s{"cfo -5 kHz"}; s.cfo = -5000; S(s); }
    { Scn s{"cfo +5 kHz, 960 ksps, snr 16 dB"}; s.cfo = 5000; s.rate = 960000; s.snr = 16; S(s); }
    { Scn s{"cfo +6 kHz"}; s.cfo = 6000; S(s); }
    { Scn s{"clock +30 ppm, cfo +1 kHz"}; s.ppm = 30; s.cfo = 1000; S(s); }
    { Scn s{"snr 14 dB"}; s.snr = 14; S(s); }
    { Scn s{"snr 10 dB, cfo -1.5 kHz"}; s.snr = 10; s.cfo = -1500; s.harsh = true; S(s); }
    { Scn s{"snr 7 dB"}; s.snr = 7; s.harsh = true; S(s); }
    { Scn s{"simulcast echo 10 us -6 dB"}; s.echoUs = 10; s.echoDb = -6; s.echoDf = 20; S(s); }
    { Scn s{"simulcast echo 30 us -6 dB"}; s.echoUs = 30; s.echoDb = -6; s.echoDf = 20; s.harsh = true; S(s); }
    { Scn s{"fading 5 Hz, snr 20"}; s.fadeHz = 5; s.snr = 20; s.harsh = true; S(s); }
    { Scn s{"fading 60 Hz, snr 20"}; s.fadeHz = 60; s.snr = 20; s.harsh = true; S(s); }
    { Scn s{"drop-out 0.3 s every 2 s"}; s.dropSec = 0.3; s.harsh = true; S(s); }
    if (table) printf("%-34s %8s %8s %7s %6s %6s %6s\n", "scenario", "lock(s)", "voice%", "ess%", "fclear", "fcalls", "calls");
    for (const Scn& s : scn) {
        if (getenv("P25P2_ONLY") && !strstr(s.name, getenv("P25P2_ONLY"))) continue;
        const Result r = run(s);
        if (table) printf("%-34s %8.2f %7.1f%% %6.1f%% %6d %6d %6s   syncs %llu unlocks %llu realigns %llu cfo %.0f\n", s.name, r.lockSec, r.voicePct, r.essPct, r.falseClear, r.falseCalls, r.callsOk ? "yes" : "NO", r.syncs, r.unlocks, r.realigns, r.cfo);
        CHECK(r.falseClear == 0, "%s: %d frames of the encrypted call handed on as clear", s.name, r.falseClear);
        if (s.noiseOnly) CHECK(r.lockSec < 0 && !r.callsOk, "%s: a sync out of noise (lock at %.2f s)", s.name, r.lockSec);
        CHECK(r.falseCalls == 0, "%s: %d calls nobody sent", s.name, r.falseCalls);
        if (!s.harsh) {
            CHECK(r.lockSec >= 0 && r.lockSec < 1.5, "%s: locked after %.2f s", s.name, r.lockSec);
            CHECK(r.callsOk, "%s: the calls were not found", s.name);
            CHECK(r.essPct > 90, "%s: ESS %.1f %%", s.name, r.essPct);
        }
    }
    return testSummary("p25p2 impair");
}
