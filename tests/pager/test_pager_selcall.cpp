// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pagers + five-tone selective calls on the same channel: CCIR, ZVEI1/2, EEA, EIA and DZVEI sequences (with repeat tones, tone length variants,
// noise, carrier offsets), alongside POCSAG; and no false calls out of POCSAG data or noise, nor with the option off.
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/tones/tones_gen.h"
#include "dect2/tones/seq_decoder.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 1.2e6;

struct Rig {
    PagerReceiver rx;
    PagerTelemetry t;
    uint64_t seq = 0;
    explicit Rig(bool sel) {
        rx.configure(kRate);
        rx.setSelcall(sel);
        rx.setSignalOffset(-pagerTuning().tuneOffsetHz);
    }
    void feed(const std::vector<cf32>& v) {
        for (size_t i = 0; i < v.size(); i += 65536) {
            rx.feed(v.data() + i, std::min<size_t>(65536, v.size() - i));
            PagerTelemetry n;
            if (rx.telemetry(n, seq)) { seq = n.seq; t = n; }
        }
    }
    void silence(double sec) {
        std::vector<cf32> v((size_t)(sec * kRate));
        std::mt19937 g(5);
        std::normal_distribution<float> d(0.f, 0.01f);
        for (auto& s : v) s = cf32(d(g), d(g));
        feed(v);
    }
    void flush() { rx.setSelcall(rx.selcall()); PagerTelemetry n; if (rx.telemetry(n, 0)) t = n; }
    std::vector<PagerMessage> calls() const {
        std::vector<PagerMessage> o;
        if (t.messages) for (const auto& m : *t.messages) if (m.selcall) o.push_back(m);
        return o;
    }
};

// one call on the air: the scene FM-modulated on the channel, the carrier at the mode's tune offset plus cfo
static std::vector<cf32> callSignal(const char* set, const std::string& digits, double toneMs, double amp, double snrDb, double cfo, uint32_t seed) {
    ToneScene s(48000, seed);
    s.pad(0.3);
    s.sequential(set, digits, 0.3, toneMs, amp);
    s.pad(s.seconds() + 0.5);
    return toneSceneFm(s, kRate, -pagerTuning().tuneOffsetHz + cfo, snrDb, seed + 100);
}

static void one(const char* set, const char* std_, const std::string& digits, double ms, double snr, double cfo, uint32_t seed, bool exact = true) {
    Rig r(true);
    r.silence(0.3);
    r.feed(callSignal(set, digits, ms, 0.3, snr, cfo, seed));
    r.silence(0.3);
    const auto c = r.calls();
    char what[96]; snprintf(what, sizeof what, "%s %s %.0f ms snr %.0f cfo %.0f", set, digits.c_str(), ms, snr, cfo);
    CHECK(c.size() == 1, "%s: %zu calls", what, c.size());
    if (c.size() == 1) {
        if (exact) CHECK(c[0].text == digits, "%s: read '%s'", what, c[0].text.c_str());
        CHECK(c[0].text.size() == digits.size(), "%s: %zu digits", what, c[0].text.size());
        if (std_) CHECK(c[0].selStd == std_, "%s: standard '%s'", what, c[0].selStd.c_str());
        CHECK(c[0].wallTime > 0 && c[0].address == 0, "%s: fields", what);
    }
}

int main() {
    // every plan at its usual tone length, with and without repeat tones
    struct P { const char* id; const char* name; double ms; };
    const P plans[] = {{"ccir", "CCIR", 100}, {"eea", "EEA", 40}, {"zvei1", "ZVEI1 / PZVEI", 70}, {"zvei2", "ZVEI2 / ZVEI3", 70}, {"eia", "EIA", 33}, {"dzvei", "DZVEI", 70}};
    // ZVEI1, ZVEI2 and DZVEI use almost the same tones (only the repeat tone and the zero differ): without a repeat tone ZVEI2 reads as ZVEI1,
    // and DZVEI is the same tone set shifted by one digit, so a DZVEI call can only be told by its length (checked, not its digits)
    for (const P& p : plans) {
        const bool amb = !strcmp(p.id, "zvei2"), dz = !strcmp(p.id, "dzvei");
        one(p.id, amb || dz ? nullptr : p.name, "14659", p.ms, 30, 0, 11, !dz);
        one(p.id, dz ? nullptr : p.name, "11223", p.ms, 30, 0, 12, !dz);       // repeat tones
        one(p.id, dz ? nullptr : p.name, "90077", p.ms, 30, 0, 13, !dz);
    }
    // the user's case: CCIR 1465
    one("ccir", "CCIR", "1465", 100, 25, 0, 21);
    // carrier offsets and noise
    for (double cfo : {-2500.0, 1800.0}) one("ccir", "CCIR", "1465", 100, 25, cfo, 22);
    for (double snr : {18.0, 14.0}) one("ccir", "CCIR", "1465", 100, snr, 300, 23);
    one("zvei1", "ZVEI1 / PZVEI", "12345", 70, 14, -700, 24);
    // tone length variants (the usual range of equipment), the digits must stay right
    for (double ms : {60.0, 80.0, 120.0, 150.0}) one("ccir", nullptr, "2780", ms, 25, 0, 25);
    for (double ms : {50.0, 100.0}) one("zvei1", nullptr, "31415", ms, 25, 0, 26);
    for (double ms : {30.0, 45.0}) one("eia", nullptr, "65432", ms, 25, 0, 27);
    // a five-tone call after a POCSAG page on the same channel: both decoded
    {
        Rig r(true);
        SynthConfig c;
        c.snrDb = 25;
        auto syn = makePagerSynthPage(c, kRate, kPocsag1200, 1234567, 0, kPagerAlpha, "HELLO POCSAG");
        std::vector<cf32> v((size_t)(kRate * 3.0));
        syn->generate(v.data(), v.size());
        r.feed(v);
        r.feed(callSignal("ccir", "1465", 100, 0.3, 25, 0, 31));
        r.silence(0.5);
        bool pocsag = false;
        if (r.t.messages) for (const auto& m : *r.t.messages) if (!m.selcall && m.text == "HELLO POCSAG" && m.address == 1234567) pocsag = true;
        CHECK(pocsag, "POCSAG page next to the selcall: missing");
        const auto c2 = r.calls();
        CHECK(c2.size() == 1 && c2[0].text == "1465", "selcall next to POCSAG: %zu calls", c2.size());
    }
    // no false calls: POCSAG data alone (with the option on), noise alone, and a call with the option off
    {
        Rig r(true);
        SynthConfig c;
        c.snrDb = 25;
        auto syn = makePagerSynth(c, kRate);
        std::vector<cf32> v((size_t)(kRate * 20.0));
        syn->generate(v.data(), v.size());
        r.feed(v);
        CHECK(r.calls().empty(), "false selcalls out of POCSAG / FLEX data: %zu", r.calls().size());
        CHECK(r.t.messagesTotal > 5, "the data was not decoded (%llu)", (unsigned long long)r.t.messagesTotal);
    }
    {
        Rig r(true);
        r.silence(20);
        CHECK(r.calls().empty(), "false selcalls out of noise");
    }
    {
        Rig r(false);
        r.feed(callSignal("ccir", "1465", 100, 0.3, 25, 0, 41));
        r.silence(0.5);
        CHECK(r.calls().empty(), "a call was decoded with the option off");
    }
    // the stand-alone decoder on audio: a gap inside the call is a different call; speech-length tones are no call
    {
        ToneScene s(8000, 3);
        s.pad(0.2);
        s.sequential("ccir", "1465", 0.2, 100, 0.3);
        s.sequential("ccir", "9021", 1.5, 100, 0.3);
        s.tone(1124, 3.0, 1.0, 0.3);                      // a long tone: no call
        s.pad(s.seconds() + 0.5);
        SeqToneDecoder d;
        std::vector<std::string> got;
        d.setCallback([&](const SeqCall& c) { got.push_back(c.digits); });
        std::vector<float> a(s.audio());
        for (auto& x : a) x *= 5000.f;                     // scene units are 5 kHz deviation
        for (size_t i = 0; i < a.size(); i += 777) d.feed(a.data() + i, std::min<size_t>(777, a.size() - i));
        CHECK(got.size() == 2 && got[0] == "1465" && got[1] == "9021", "stand-alone decoder: %zu calls", got.size());
    }
    return testSummary("pager_selcall");
}
