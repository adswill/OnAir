// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode S uplink receiver on the test signal, with impairments: every uplink format through the DPSK demodulator, noise near the threshold,
// +-200 kHz offsets, overlapping interrogations of two interrogators, Mode A/C against Mode S, sidelobe P2 and P5, intermode all-calls,
// and the test scene: interrogator codes, antenna rotation periods from the main-beam bursts, aircraft and their registers, ACAS.
#include "dect2/uplink/uplink_gen.h"
#include "dect2/uplink/uplink_rx.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Run {
    std::vector<UplinkEvent> ev;
    UplinkTelemetry tel;
};

static Run run(const std::vector<UplinkTx>& txs, double seconds, double rate = 10e6, double snrDb = 30, double cfo = 0, uint32_t seed = 1) {
    const double main = -12;
    UplinkGenerator g(rate, std::pow(10.0, (main - snrDb) / 20.0), cfo, seed);
    for (const auto& t : txs) g.add(t);
    UplinkReceiver r;
    r.configure(rate);
    Run out;
    r.setEventCallback([&](const UplinkEvent& e) { out.ev.push_back(e); });
    std::vector<cf32> buf(65536);
    for (double t = 0; t < seconds; t += buf.size() / rate) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    r.flush();
    r.telemetry(out.tel, ~0ull);
    return out;
}

static UplinkTx modeS(double sec, const uplink::Frame& f, float level = -12, float phase = 0) {
    UplinkTx x; x.sec = sec; x.kind = 1; x.p2Db = 0; x.levelDb = level; x.frame = f; x.phase = phase; return x;
}
static UplinkTx modeAc(double sec, char m, float p2Db, int p4 = 0, float level = -12, float phase = 0) {
    UplinkTx x; x.sec = sec; x.kind = 0; x.acMode = m; x.p2Db = p2Db; x.p4 = p4; x.levelDb = level; x.phase = phase; return x;
}

static std::vector<uplink::Frame> allFormats(uint32_t i) {
    using namespace uplink;
    const uint32_t a = (0x4CA2B1 + i * 0x10101) & 0xFFFFFF;
    uint8_t ma[7], mc[10], mu[7];
    for (int k = 0; k < 7; k++) ma[k] = (uint8_t)(i * 31 + k * 17);
    for (int k = 0; k < 10; k++) mc[k] = (uint8_t)(i * 13 + k * 29);
    raMu(mu, 0x3C6586, 0, 0, 1 + (i & 1), 0, 0, 0, 0);
    return {uf0(a, 0, 1, 0), uf45(4, a, 0, 20, 7, sdIis(3, 0)), uf45(5, a, 0, 17, 3, sdSis(17, 0, 2)), uf11(i & 7, i & 15, (i >> 4) % 5),
            uf16(a, 0, 0, mu), uf2021(20, a, 0, 21, 7, sdIis(1, 0), ma), uf2021(21, a, 0, 22, 0, sdIis(2, 0), ma), uf24(a, 1, i & 15, mc)};
}

// n rounds of every format, 100 us apart; returns the fraction decoded with exactly the bits sent
static double formats(const char* name, double snrDb, double cfo, int rounds, double rate = 10e6) {
    std::vector<UplinkTx> txs;
    std::vector<uplink::Frame> sent;
    double t = 0.001;
    for (int i = 0; i < rounds; i++)
        for (const auto& f : allFormats(i)) { txs.push_back(modeS(t, f, -12, (float)(i * 0.7))); sent.push_back(f); t += 100e-6; }
    Run r = run(txs, t + 0.001, rate, snrDb, cfo, 7);
    size_t ok = 0, j = 0;
    double cfoSum = 0;
    for (const auto& e : r.ev) {
        if (e.kind != 2) continue;
        while (j < txs.size() && txs[j].sec < e.sec - 1e-6) j++;
        if (j < txs.size() && std::fabs(txs[j].sec - e.sec) < 1e-6 && e.msg.nbits == sent[j].nbits && !memcmp(e.msg.b, sent[j].b, sent[j].nbits / 8)) { ok++; cfoSum += e.cfoHz; }
        else if (getenv("UPLINK_BITS") && j < txs.size()) { printf("    UF%d:", sent[j].b[0] >> 3); for (int k = 0; k < sent[j].nbits; k++) if (((e.msg.b[k / 8] ^ sent[j].b[k / 8]) >> (7 - k % 8)) & 1) printf(" %d", k + 1); printf("\n"); }
    }
    const double frac = (double)ok / sent.size();
    int kinds[5] = {};
    for (const auto& e : r.ev) kinds[e.kind]++;
    if (frac < 1) printf("    events: A/C %d, intermode %d, ok %d, no sync %d, bad %d\n", kinds[0], kinds[1], kinds[2], kinds[3], kinds[4]);
    printf("  %s: %zu of %zu decoded (%.1f %%), offset measured %.1f kHz\n", name, ok, sent.size(), 100 * frac, ok ? cfoSum / ok / 1e3 : 0.0);
    if (ok && std::fabs(cfo) > 0) CHECK(std::fabs(cfoSum / ok - cfo) < 20e3, "%s: offset %.0f Hz", name, cfoSum / ok);
    return frac;
}

int main() {
    CHECK(formats("all formats, 30 dB", 30, 0, 40) == 1.0, "every format at 30 dB");
    CHECK(formats("8 Msps", 30, 0, 20, 8e6) == 1.0, "every format at 8 Msps");
    CHECK(formats("+200 kHz", 25, 200e3, 20) == 1.0, "+200 kHz");
    CHECK(formats("-200 kHz", 25, -200e3, 20) == 1.0, "-200 kHz");
    CHECK(formats("15 dB", 15, 50e3, 40) >= 0.94, "15 dB");
    CHECK(formats("12 dB (near threshold)", 12, 0, 40) >= 0.85, "12 dB");
    CHECK(formats("9 dB (under it)", 9, 0, 20) < 0.85, "9 dB is not expected to decode well");

    {   // Mode A/C against Mode S, P2 and P5 sidelobes, intermode all-calls
        std::vector<UplinkTx> v;
        v.push_back(modeAc(0.001, 'A', -12));            // main beam
        v.push_back(modeAc(0.002, 'C', -99));            // no P2
        v.push_back(modeAc(0.003, 'A', 3));              // sidelobe: P2 over P1
        v.push_back(modeAc(0.004, 'C', 0));              // sidelobe: P2 = P1
        v.push_back(modeAc(0.005, 'A', -12, 2));         // A/C/S all-call
        v.push_back(modeAc(0.006, 'C', -12, 1));         // A/C-only all-call
        UplinkTx s = modeS(0.007, uplink::uf11(0, 3, 0)); s.p5 = true; v.push_back(s);   // Mode S sidelobe: P5 over the sync
        v.push_back(modeS(0.008, uplink::uf11(0, 3, 0)));
        Run r = run(v, 0.01);
        CHECK(r.ev.size() == 8, "%zu events", r.ev.size());
        if (r.ev.size() == 8) {
            CHECK(r.ev[0].kind == 0 && r.ev[0].acMode == 'A' && !r.ev[0].sidelobe && std::fabs(r.ev[0].p2Db + 12) < 1.5, "Mode A main beam (P2 %.1f)", r.ev[0].p2Db);
            CHECK(r.ev[1].kind == 0 && r.ev[1].acMode == 'C' && !r.ev[1].sidelobe, "Mode C, no P2");
            CHECK(r.ev[2].kind == 0 && r.ev[2].acMode == 'A' && r.ev[2].sidelobe, "Mode A sidelobe");
            CHECK(r.ev[3].kind == 0 && r.ev[3].acMode == 'C' && r.ev[3].sidelobe, "Mode C sidelobe");
            CHECK(r.ev[4].kind == 1 && r.ev[4].p4 == 2 && r.ev[4].acMode == 'A', "A/C/S all-call");
            CHECK(r.ev[5].kind == 1 && r.ev[5].p4 == 1 && r.ev[5].acMode == 'C', "A/C-only all-call");
            CHECK(r.ev[6].kind == 3, "Mode S with P5: kind %d", r.ev[6].kind);
            CHECK(r.ev[7].kind == 2 && r.ev[7].msg.uf == 11 && r.ev[7].msg.ii == 3, "Mode S UF11");
        }
        CHECK(r.tel.modeA == 2 && r.tel.modeC == 2 && r.tel.modeAcSidelobe == 2 && r.tel.intermodeAcs == 1 && r.tel.intermodeAc == 1 && r.tel.modeSSidelobe == 1 && r.tel.modeS == 1,
              "counts A %llu C %llu", (unsigned long long)r.tel.modeA, (unsigned long long)r.tel.modeC);
    }
    {   // two interrogators overlapping: Mode A/C pulses interleaved, and a weaker Mode A/C over a Mode S P6
        std::vector<UplinkTx> v;
        v.push_back(modeAc(0.001, 'A', -99, 0, -12, 0));
        v.push_back(modeAc(0.001 + 4.3e-6, 'C', -99, 0, -20, 2.0f));        // its P1 between the first's P1 and P3, its P3 after
        v.push_back(modeS(0.002, uplink::uf45(4, 0x4CA2B1, 0, 20, 7, uplink::sdIis(3, 0)), -12, 0));
        v.push_back(modeAc(0.002 + 9e-6, 'A', -99, 0, -26, 1.0f));          // inside P6, 14 dB weaker
        v.push_back(modeS(0.003, uplink::uf11(0, 5, 0), -12, 0));
        v.push_back(modeS(0.003 + 25e-6, uplink::uf11(0, 9, 0), -18, 1.3f));  // a second all-call just after the first one ends
        Run r = run(v, 0.005);
        int a = 0, c = 0, s4 = 0, ii5 = 0, ii9 = 0;
        for (const auto& e : r.ev) {
            if (e.kind == 0) { a += e.acMode == 'A'; c += e.acMode == 'C'; }
            if (e.kind == 2 && e.msg.uf == 4 && e.msg.addr == 0x4CA2B1) s4++;
            if (e.kind == 2 && e.msg.uf == 11) { ii5 += e.msg.ii == 5; ii9 += e.msg.ii == 9; }
        }
        CHECK(a >= 1 && c == 1, "interleaved Mode A/C: A %d C %d", a, c);
        CHECK(s4 == 1, "UF4 under a weaker Mode A");
        CHECK(ii5 == 1 && ii9 == 1, "two all-calls back to back: II 5 %d, II 9 %d", ii5, ii9);
    }
    {   // the test scene: interrogators, rotation, aircraft, ACAS (8 Msps, the lowest rate)
        std::vector<UplinkTx> v;
        uplinkScene(0, 21, v, -12);
        Run r = run(v, 21, 8e6, 30, 30e3, 11);
        const UplinkInterrogator *ii3 = nullptr, *si17 = nullptr, *ac = nullptr;
        for (const auto& i : r.tel.interrogators) { if (i.ii == 3) ii3 = &i; if (i.si == 17) si17 = &i; if (i.kind == 1) ac = &i; }
        CHECK(ii3 && std::fabs(ii3->periodSec - 4.8) < 0.1, "II 3 rotation %.2f s", ii3 ? ii3->periodSec : 0.f);
        CHECK(si17 && std::fabs(si17->periodSec - 10) < 0.1, "SI 17 rotation %.2f s", si17 ? si17->periodSec : 0.f);
        CHECK(ac && std::fabs(ac->periodSec - 6) < 0.15 && ac->sidelobe > 50, "Mode A/C rotation %.2f s, %llu sidelobe", ac ? ac->periodSec : 0.f, ac ? (unsigned long long)ac->sidelobe : 0ull);
        if (ii3) printf("  II 3: %llu, %.1f /s, %.2f s, %.1f dBFS\n", (unsigned long long)ii3->count, ii3->ratePerSec, ii3->periodSec, ii3->levelDb);
        const UplinkAircraft* x = nullptr;
        for (const auto& a : r.tel.aircraft) if (a.addr == 0x3C6586) x = &a;
        CHECK(x && x->confirmed && x->uf[0] >= 18 && x->uf[16] >= 3 && !x->registers.empty(), "aircraft 3C6586");
        if (x) printf("  3C6586: %llu, registers %zu, %s\n", (unsigned long long)x->count, x->registers.size(), x->interrogators.c_str());
        int ra = 0;
        for (const auto& t : r.tel.tcas) ra += t.uf == 16 && t.from == 0x4CA2B1 && t.to == 0x3C6586;
        CHECK(ra >= 3, "RA coordination %d", ra);
        bool farBy = false;
        for (const auto& a : r.tel.aircraft) farBy |= a.addr == 0x40621D;
        CHECK(farBy, "roll-call through a sidelobe");
        CHECK(r.tel.blocksBad == 0 || r.tel.blocksBad < r.tel.blocksOk / 50, "bad %llu of %llu", (unsigned long long)r.tel.blocksBad, (unsigned long long)r.tel.blocksOk);
        printf("  scene: %s\n", uplinkSummary(r.tel).c_str());
    }
    {   // noise only: nothing decoded
        Run r = run({}, 0.5, 10e6, 30);
        CHECK(r.tel.blocksOk == 0, "noise: %llu decoded", (unsigned long long)r.tel.blocksOk);
    }
    printf(fails ? "uplink rx: %d FAILED\n" : "uplink rx: all passed\n", fails);
    return fails ? 1 : 0;
}
