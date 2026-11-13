// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SITOR-A / AMTOR ARQ monitor (sitor/sitor_arq.h) through the SITOR receiver (sitor/sitor_live.h): generated links with the call, repeats
// asked by the receiving station, a change of direction and the end of communication; carrier offsets, tone polarity, a noise sweep,
// drift, a weak or missing control signal, chunk sizes, and SITOR-B next to it (neither may disturb the other). Simulated signals only.
#include "dect2/marine/marine_navtex.h"
#include "dect2/sitor/sitor_arq.h"
#include "dect2/sitor/sitor_live.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

namespace {

const char* kText = "CQ DE TEST VESSEL. POSITION 54 30N 010 12E. ALL WELL 1234567890 /";
const char* kText2 = "QSL TNX FOR TRAFFIC 73";

uint8_t pattern(const char* by) {   // "BYBYYBB" -> code, first element = least significant bit, B = 1
    uint8_t v = 0;
    for (int i = 0; i < 7; i++) if (by[i] == 'B') v |= static_cast<uint8_t>(1u << i);
    return v;
}

struct Sc {
    double rate = 250e3, snr = 30, cfo = 0, drift = 0;
    size_t chunk = 65536;
    bool invert = false;
    sitor::ArqGenCfg g;
};

size_t count(const std::string& s, const std::string& sub) {
    size_t n = 0;
    for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + 1)) n++;
    return n;
}

SitorTelemetry run(const Sc& s, const std::string& text) {
    sitor::ArqGenCfg g = s.g;
    g.rate = s.rate;
    g.centreHz = -sitorTuning().tuneOffsetHz + s.cfo;
    g.driftHzPerSec = s.drift;
    std::vector<cf32> x = sitor::genSitorArq(g, text);
    std::mt19937 rng(7);
    const double sigma = std::sqrt(std::pow(10.0, -s.snr / 10.0) * s.rate / 3000.0 / 2.0);   // SNR of station A in 3 kHz
    std::normal_distribution<float> nd(0.f, static_cast<float>(sigma));
    for (auto& v : x) {
        v = cf32(v.real() + nd(rng), v.imag() + nd(rng));
        if (s.invert) v = std::conj(v);
        v *= 0.2f;
    }
    SitorLiveReceiver rx;
    rx.configure(s.rate);
    rx.setSignalOffset(s.invert ? sitorTuning().tuneOffsetHz : -sitorTuning().tuneOffsetHz);
    SitorTelemetry t, last;
    uint64_t seq = 0;
    for (size_t k = 0; k < x.size(); k += s.chunk) {
        rx.feed(x.data() + k, std::min(s.chunk, x.size() - k));
        while (rx.telemetry(t, seq)) { seq = t.seq; last = t; }
    }
    return last;
}

int blocksOf(const std::string& text) { return static_cast<int>(sitor::arqWords(text).size() / 3); }

void want(const char* what, const Sc& s, bool exact = true) {
    const SitorTelemetry t = run(s, kText);
    const bool has = count(t.arqText, kText) == 1;
    const int repeatsSent = s.g.repeatEvery > 0 ? blocksOf(kText) / (s.g.repeatEvery - 1) : 0;
    printf("%-34s arq %d, %llu new, %llu repeats (about %d sent), %llu bad, %llu CS at %d, call \"%s\": %s\n", what, t.arqLocked ? 1 : 0,
           static_cast<unsigned long long>(t.arqNew), static_cast<unsigned long long>(t.arqRepeats), repeatsSent, static_cast<unsigned long long>(t.arqBad),
           static_cast<unsigned long long>(t.arqCs), t.arqCsOffset, t.selcall.c_str(), has ? "text ok" : "TEXT WRONG");
    if (exact) {
        CHECK(has, "%s: text \"%.120s\"", what, t.arqText.c_str());
        CHECK(t.selcall == s.g.selcall, "%s: selcall \"%s\"", what, t.selcall.c_str());
        CHECK(t.arqLog.find("end of communication") != std::string::npos, "%s: no end of communication in \"%s\"", what, t.arqLog.c_str());
        CHECK(t.arqRepeats >= 1, "%s: no repeat recognised", what);
        CHECK(t.state != 2, "%s: SITOR-B locked on an ARQ link", what);
    } else {
        CHECK(t.arqText.find("POSITION 54 30N") != std::string::npos, "%s: text \"%.120s\"", what, t.arqText.c_str());
    }
}

}  // namespace

int main() {
    // the service signals are the table 2 patterns, and the control signals are 4-of-7 words like the traffic signals
    CHECK(sitor::kCs1 == pattern("BYBYYBB") && sitor::kCs2 == pattern("YBYBYBB") && sitor::kCs3 == pattern("BYYBBYB"), "control signals");
    CHECK(sitor::kArqBeta == pattern("BBYYBBY") && sitor::kArqAlpha == pattern("BBBBYYY") && sitor::kArqRq == pattern("YBBYYBB"), "idle and RQ");
    CHECK(sitor::kArqBeta == marine::kBeta && sitor::kArqAlpha == marine::kAlpha && sitor::kArqRq == marine::kRep, "same as the FEC table");
    { uint8_t l = 0; CHECK(marine::ccir476Code('L', false, l) && l == sitor::kCs1, "CS1 is the pattern of L (table 1)"); }
    { uint8_t a = 0; CHECK(marine::ccir476Code('A', false, a) && a == pattern("BBBYYYB"), "A = BBBYYYB"); }
    CHECK(marine::ccir476Valid(sitor::kCs1) && marine::ccir476Valid(sitor::kCs2) && marine::ccir476Valid(sitor::kCs3), "4 of 7");
    CHECK(sitor::arqWords("AB").size() % 3 == 0, "blocks of three");

    want("clean, 250 ksps", Sc());
    { Sc s; s.rate = 2e6; want("2 Msps", s); }
    for (double f : {-60.0, 45.0, 150.0}) { Sc s; s.cfo = f; char n[48]; snprintf(n, sizeof n, "carrier offset %+.0f Hz", f); want(n, s); }
    { Sc s; s.invert = true; want("tones swapped (LSB)", s); }
    for (double snr : {15.0, 8.0, 4.0}) { Sc s; s.snr = snr; char n[48]; snprintf(n, sizeof n, "SNR %.0f dB in 3 kHz", snr); want(n, s); }
    { Sc s; s.snr = 0; want("SNR 0 dB (partial allowed)", s, false); }
    { Sc s; s.drift = 1.0; want("drift 1 Hz/s", s); }
    { Sc s; s.g.irsLevel = 0.1; s.snr = 25; want("weak IRS (-20 dB)", s); }
    { Sc s; s.g.irsDelayBits = 3.0; want("short round trip", s); }
    { Sc s; s.g.irsDelayBits = 15.6; want("long round trip", s); }
    { Sc s; s.g.repeatEvery = 3; want("repeat every 3rd block", s); }
    { Sc s; s.g.irsOffsetHz = 25; want("IRS 25 Hz off", s); }
    for (size_t c : {size_t(5), size_t(1000)}) { Sc s; s.chunk = c; char n[40]; snprintf(n, sizeof n, "chunks of %zu", c); want(n, s); }
    { Sc s; s.g.call = false; s.g.selcall = ""; const SitorTelemetry t = run(s, kText);
      CHECK(count(t.arqText, kText) == 1, "no call: text \"%.100s\"", t.arqText.c_str()); }
    // the control signal is not heard at all: repeats are found by comparing the blocks
    { Sc s; s.g.irsLevel = 0; const SitorTelemetry t = run(s, kText);
      printf("%-34s %llu new, %llu repeats, CS %llu\n", "IRS not heard", static_cast<unsigned long long>(t.arqNew), static_cast<unsigned long long>(t.arqRepeats), static_cast<unsigned long long>(t.arqCs));
      CHECK(count(t.arqText, kText) == 1, "IRS not heard: text \"%.120s\"", t.arqText.c_str()); }
    // change of direction: "+?", CS3, beta alpha beta, then the other station's text
    { Sc s; s.g.changeOver = true; s.g.text2 = kText2; const SitorTelemetry t = run(s, kText);
      printf("%-34s text \"%s\"\n%s", "change of direction", t.arqText.c_str(), t.arqLog.c_str());
      CHECK(count(t.arqText, std::string(kText) + "+?") == 1, "change-over: first text");
      CHECK(count(t.arqText, kText2) == 1, "change-over: second text \"%s\"", t.arqText.c_str());
      CHECK(t.arqLog.find("CS3") != std::string::npos && t.arqLog.find("other station is sending") != std::string::npos, "change-over events"); }
    // noise only: no lock
    { Sc s; s.snr = -30; s.g.call = false; s.g.repeatEvery = 0; const SitorTelemetry t = run(s, "");
      CHECK(t.arqNew <= 2 && t.arqText.size() < 6, "noise: %llu blocks, \"%s\"", static_cast<unsigned long long>(t.arqNew), t.arqText.c_str()); }
    return testSummary("sitor_arq");
}
