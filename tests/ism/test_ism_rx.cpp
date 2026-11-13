// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors receiver over the air (synthetic): the test scenes through the receiver with the faults of tests/common/impair.h and the ones of
// the band itself. Every reading must be one of the test devices with its right values (a wrong reading is a failure in every case);
// the clean cases must hear every device.
//
//   case                         what must hold
//   clean 433 / 868 scenes       every device heard, nothing wrong
//   +-50 kHz offset              every device heard (cheap sensors drift that far, on top of their own offsets)
//   clock +-200 ppm              every device heard
//   12 dB C/N (100 kHz)          most devices heard; 6 dB: nothing wrong
//   two at once, 200 kHz apart   both heard; on the same frequency: nothing wrong
//   carrier 30 dB, 300 kHz away  every device heard
//   cut-off packets              nothing wrong
//   noise, zeros, NaN            nothing reported
#include "dect2/ism/ism_gen.h"
#include "dect2/ism/ism_rx.h"
#include "dect2/ism/ism_tel.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <set>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRate = 2e6, kDial = -250e3;

struct Result { std::set<int> heard; int readings = 0, wrong = 0; IsmTelemetry tel; };

static Result run(const std::vector<cf32>& x) {
    Result r;
    IsmReceiver rx;
    rx.configure(kRate);
    rx.setSignalOffset(kDial);
    rx.setReadingTap([&](const IsmReading& rd, double) {
        r.readings++;
        IsmTx w;
        if (ismTruthMatches(rd, &w)) r.heard.insert((int)w);
        else { r.wrong++; printf("    WRONG: %s id %s: %s\n", rd.model.c_str(), rd.id.c_str(), ismReadingText(rd.fields).c_str()); }
    });
    const size_t chunk = 65536;
    for (size_t i = 0; i < x.size(); i += chunk) rx.feed(x.data() + i, std::min(chunk, x.size() - i));
    std::vector<cf32> z(65536, cf32(0, 0));   // let the last burst end
    rx.feed(z.data(), z.size());
    rx.telemetry(r.tel, 0);
    if (getenv("ISM_RAW"))
        for (const auto& w : r.tel.raws) {
            printf("    raw %.3f s %+.1f kHz %s %s %zu pulses, %.0f/%.0f:", w.sec, w.freqHz / 1e3, w.fsk ? "FSK" : "OOK", w.guess.code.c_str(), w.pulses.size(), w.guess.shortUs, w.guess.longUs);
            for (size_t i = 0; i < w.pulses.size() && i < (getenv("ISM_RAWN") ? (size_t)atoi(getenv("ISM_RAWN")) : 24); i++) printf(" %.0f/%.0f", w.pulses[i].pulseUs, w.pulses[i].gapUs);
            printf("\n");
        }
    return r;
}

static std::vector<cf32> make(const IsmGenConfig& c, double sec) {
    IsmGenerator g(c);
    std::vector<cf32> x((size_t)(sec * c.rate));
    g.generate(x.data(), x.size());
    return x;
}

static IsmGenConfig scene(int s, double snr = 30) {
    IsmGenConfig c;
    c.rate = kRate; c.dialOffsetHz = kDial; c.snrDb = snr; c.seed = 11 + s;
    c.devs = ismTestScene(s);
    return c;
}

static std::string missing(const Result& r, const std::vector<IsmGenDevice>& devs) {
    std::string m;
    for (const auto& d : devs) if (!r.heard.count((int)d.proto)) { if (!m.empty()) m += ", "; m += ismTxName(d.proto); }
    return m.empty() ? "-" : m;
}

static void row(const char* name, const Result& r, size_t n) {
    printf("  %-30s %2zu/%2zu heard, %3d readings, %d wrong, %llu unknown bursts\n", name, r.heard.size(), n, r.readings, r.wrong, (unsigned long long)r.tel.blocksBad);
}

int main() {
    const auto s0 = ismTestScene(0), s1 = ismTestScene(1);
    {
        const Result r = run(make(scene(0, getenv("ISM_SNR") ? atof(getenv("ISM_SNR")) : 30), 5.4));
        row("clean, 433.92 MHz scene", r, s0.size());
        CHECK(r.heard.size() == s0.size() && r.wrong == 0, "433 scene: missing %s, %d wrong", missing(r, s0).c_str(), r.wrong);
        CHECK(r.tel.devices.size() == s0.size(), "%zu devices in the table", r.tel.devices.size());
        bool freqOk = true;
        for (const auto& d : r.tel.devices) {   // the offsets from the dial, as no radio frequency is known here
            for (const auto& g : s0) {
                IsmReading rd; rd.model = d.model; rd.id = d.id; rd.fields = d.fields;
                IsmTx w;
                if (ismTruthMatches(rd, &w) && w == g.proto && std::fabs(d.freqHz - g.offsetHz) > 8e3) { freqOk = false; printf("    %s at %.1f kHz, sent at %.1f\n", d.model.c_str(), d.freqHz / 1e3, g.offsetHz / 1e3); }
            }
        }
        CHECK(freqOk, "device frequencies");
        if (getenv("ISM_FIRST")) return fails;
    }
    {
        const Result r = run(make(scene(1), 4.6));
        row("clean, 868 MHz scene", r, s1.size());
        CHECK(r.heard.size() == s1.size() && r.wrong == 0, "868 scene: missing %s, %d wrong", missing(r, s1).c_str(), r.wrong);
    }
    {   // Z-Wave R1 / R2 / R3 on 868.4 MHz: clean, +-20 kHz offset, 12 dB, noise
        const auto s2 = ismTestScene(2);
        auto zc = [&](double snr) { IsmGenConfig c = scene(2, snr); return c; };
        const Result r = run(make(zc(30), 5.4));
        row("Z-Wave clean", r, s2.size());
        CHECK(r.heard.size() == s2.size() && r.wrong == 0, "zwave: missing %s, %d wrong", missing(r, s2).c_str(), r.wrong);
        for (double off : {20e3, -20e3}) {
            auto x = make(zc(30), 5.4);
            impair::shift(x, off, kRate);
            const Result ro = run(x);
            char n[64]; snprintf(n, sizeof n, "Z-Wave offset %+.0f kHz", off / 1e3);
            row(n, ro, s2.size());
            CHECK(ro.heard.size() == s2.size() && ro.wrong == 0, "%s: missing %s, %d wrong", n, missing(ro, s2).c_str(), ro.wrong);
        }
        const Result r16 = run(make(zc(16), 5.4));
        row("Z-Wave 16 dB C/N", r16, s2.size());
        CHECK(r16.heard.size() == s2.size() && r16.wrong == 0, "zwave 16 dB: missing %s, %d wrong", missing(r16, s2).c_str(), r16.wrong);
        const Result r12 = run(make(zc(12), 16.0));
        row("Z-Wave 12 dB C/N", r12, s2.size());
        CHECK(r12.heard.size() >= 1 && r12.wrong == 0, "zwave 12 dB (marginal: R1 and R3 need about 16 dB): missing %s, %d wrong", missing(r12, s2).c_str(), r12.wrong);
        IsmGenConfig nz = zc(20);
        nz.devs.clear();
        const Result rn = run(make(nz, 3.0));
        CHECK(rn.readings == 0, "zwave noise: %d readings", rn.readings);
    }
    for (double off : {50e3, -50e3}) {
        auto x = make(scene(0), 5.4);
        impair::shift(x, off, kRate);
        const Result r = run(x);
        char n[64]; snprintf(n, sizeof n, "offset %+.0f kHz", off / 1e3);
        row(n, r, s0.size());
        CHECK(r.heard.size() == s0.size() && r.wrong == 0, "%s: missing %s, %d wrong", n, missing(r, s0).c_str(), r.wrong);
    }
    for (double ppm : {200.0, -200.0}) {
        auto c = scene(1);
        auto x = impair::clock(make(c, 4.6), ppm);
        const Result r = run(x);
        char n[64]; snprintf(n, sizeof n, "clock %+.0f ppm (868)", ppm);
        row(n, r, s1.size());
        CHECK(r.heard.size() == s1.size() && r.wrong == 0, "%s: missing %s, %d wrong", n, missing(r, s1).c_str(), r.wrong);
    }
    {
        auto c = scene(0);
        c.devs.resize(7);   // the OOK weather stations
        auto x = impair::clock(make(c, 4.0), 200);
        const Result r = run(x);
        row("clock +200 ppm (433 OOK)", r, c.devs.size());
        CHECK(r.heard.size() == c.devs.size() && r.wrong == 0, "clock 433: missing %s, %d wrong", missing(r, c.devs).c_str(), r.wrong);
    }
    {
        const Result r = run(make(scene(0, 12), 5.4));
        row("12 dB C/N", r, s0.size());
        CHECK(r.heard.size() >= s0.size() - 3 && r.wrong == 0, "12 dB: missing %s, %d wrong", missing(r, s0).c_str(), r.wrong);
        auto c1 = scene(1, 12);
        const Result r1 = run(make(c1, 4.6));
        row("12 dB C/N (868)", r1, s1.size());
        CHECK(r1.heard.size() >= s1.size() - 1 && r1.wrong == 0, "12 dB 868: missing %s, %d wrong", missing(r1, s1).c_str(), r1.wrong);
        const Result r6 = run(make(scene(0, 6), 5.4));
        row("6 dB C/N", r6, s0.size());
        CHECK(r6.wrong == 0, "6 dB: %d wrong", r6.wrong);
    }
    {
        IsmGenConfig c = scene(0);
        c.devs.clear();
        IsmGenDevice a; a.proto = IsmTx::Nexus; a.offsetHz = -30e3; a.phase = 0.2; a.period = 100; c.devs.push_back(a);
        IsmGenDevice b; b.proto = IsmTx::AcuriteTower; b.offsetHz = 200e3; b.phase = 0.4; b.period = 100; c.devs.push_back(b);
        IsmGenDevice e; e.proto = IsmTx::Bresser6in1; e.offsetHz = -230e3; e.phase = 0.5; e.period = 100; c.devs.push_back(e);
        const Result r = run(make(c, 1.3));
        row("three at once, 200 kHz apart", r, c.devs.size());
        CHECK(r.heard.size() == 3 && r.wrong == 0, "apart: missing %s, %d wrong", missing(r, c.devs).c_str(), r.wrong);
        IsmGenConfig d = scene(0);
        d.devs.clear();
        IsmGenDevice f; f.proto = IsmTx::Tx141th; f.offsetHz = 40e3; f.phase = 0.2; f.period = 100; d.devs.push_back(f);
        IsmGenDevice g; g.proto = IsmTx::F007th; g.offsetHz = 55e3; g.phase = 0.25; g.period = 100; d.devs.push_back(g);
        IsmGenDevice h; h.proto = IsmTx::Toyota; h.offsetHz = 20e3; h.phase = 0.3; h.period = 100; h.levelDb = -3; d.devs.push_back(h);
        IsmGenDevice k; k.proto = IsmTx::Ev1527; k.offsetHz = 0; k.phase = 0.32; k.period = 100; k.levelDb = -20; d.devs.push_back(k);
        const Result q = run(make(d, 1.2));
        row("colliding on one frequency", q, d.devs.size());
        CHECK(q.wrong == 0, "colliding: %d wrong", q.wrong);
    }
    {
        IsmGenConfig c = scene(0);
        c.carrierHz = 300e3; c.carrierDb = 30;
        const Result r = run(make(c, 5.4));
        row("carrier +30 dB, 300 kHz away", r, s0.size());
        CHECK(r.heard.size() == s0.size() && r.wrong == 0, "carrier: missing %s, %d wrong", missing(r, s0).c_str(), r.wrong);
        CHECK(r.tel.blocksBad <= 2, "carrier: %llu unknown bursts", (unsigned long long)r.tel.blocksBad);
    }
    {
        int wrong = 0, readings = 0;
        for (double cut : {0.3, 0.5, 0.7, 0.85, 0.95}) {
            IsmGenConfig c = scene(0);
            for (auto& d : c.devs) d.truncate = cut;
            const Result r = run(make(c, 5.4));
            IsmGenConfig c1 = scene(1);
            for (auto& d : c1.devs) d.truncate = cut;
            const Result r1 = run(make(c1, 4.6));
            wrong += r.wrong + r1.wrong;
            readings += r.readings + r1.readings;
            printf("  cut off at %3.0f %%                %2zu/%2zu heard (whole packets inside the burst), %d wrong\n", cut * 100, r.heard.size() + r1.heard.size(), s0.size() + s1.size(), r.wrong + r1.wrong);
        }
        CHECK(wrong == 0, "cut-off packets: %d wrong of %d readings", wrong, readings);
    }
    {
        IsmGenConfig c = scene(0, 20);
        c.devs.clear();
        const Result r = run(make(c, 3.0));
        row("noise only", r, 0);
        CHECK(r.readings == 0 && r.tel.blocksBad <= 1, "noise: %d readings, %llu unknown", r.readings, (unsigned long long)r.tel.blocksBad);
        std::vector<cf32> z(4000000, cf32(0, 0));
        for (size_t i = 0; i < z.size(); i += 997) z[i] = cf32(NAN, INFINITY);
        const Result rz = run(z);
        CHECK(rz.readings == 0, "zeros / NaN: %d readings", rz.readings);
    }
    printf(fails ? "ism rx: %d FAILED\n" : "ism rx: all passed\n", fails);
    return fails ? 1 : 0;
}
