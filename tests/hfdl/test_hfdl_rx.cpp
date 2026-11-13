// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HFDL receiver loopback: known squitters and ACARS / performance data bursts at every rate, through the receiver, with a tuning error of
// +-100 / +-200 Hz, a sample clock off by +-100 ppm, timing offsets, noise, a two-path HF channel with delay and Doppler spread, several channels at once, and noise alone.
#include "dect2/hfdl/hfdl_gen.h"
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/dsp/portable_rng.h"
#include <chrono>
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 48000, kCentre = 8.935e6;
static const double kChans[3] = {8.927e6, 8.942e6, 8.936e6};

static double gPpm = 0;     // transmitter / ADC clock error: the signal is rendered at kRate * (1 + gPpm 1e-6)

struct Tx { int chan; int m1; std::vector<uint8_t> pdu; double t; double cfo; double levelDb = 0; };

// plays the bursts (with noise at snrDb Es/N0 of a 0 dB burst) through a receiver; returns its telemetry
static HfdlTelemetry play(const std::vector<Tx>& txs, double snrDb, const std::vector<HfdlPath>& paths, double secs, uint32_t seed = 1) {
    HfdlReceiver rx;
    rx.configure(kRate);
    rx.setCenterHz(kCentre);
    rx.setChannels({kChans[0], kChans[1], kChans[2]});
    std::vector<HfdlWave> waves;
    const double amp = 0.1, fsEff = kRate * (1 + gPpm * 1e-6);
    for (size_t i = 0; i < txs.size(); i++) {
        HfdlWave w = hfdlMakeWave(txs[i].m1, txs[i].pdu, paths, seed * 31 + (uint32_t)i);
        w.start = txs[i].t * fsEff;
        w.carHz = kChans[txs[i].chan] + kHfdlCarrierHz - kCentre + txs[i].cfo;
        w.amp = amp * std::pow(10.0, txs[i].levelDb / 20);
        w.phase = 0.3 + i;
        waves.push_back(std::move(w));
    }
    const double sigma = snrDb >= 99 ? 0 : std::sqrt(amp * amp / std::pow(10.0, snrDb / 10) * (kRate / kHfdlBaud) / 2);
    PortableRng rng(seed);   // the same noise on every platform
    const size_t N = 4096;
    std::vector<cf32> buf(N);
    for (int64_t pos = 0; pos < (int64_t)(secs * kRate); pos += (int64_t)N) {
        for (auto& v : buf) v = sigma > 0 ? cf32((float)rng.gauss(sigma), (float)rng.gauss(sigma)) : cf32(0, 0);
        for (const auto& w : waves) hfdlRender(w, buf.data(), pos, N, fsEff);
        rx.feed(buf.data(), N);
    }
    HfdlTelemetry t;
    rx.telemetry(t, ~0ull);
    return t;
}

static std::vector<uint8_t> acarsPdu(int m1) {
    const std::string text = m1 == 0 || m1 == 4 ? "OK" : "#DFB TEST MESSAGE FROM HFDL LOOPBACK FL350";
    if (m1 == 0 || m1 == 4) return hfdlMpduDown(1, 23, {hfdlUnnumbered(hfdlAcarsHfnpdu("N1UA", "H1", '2', text, true))});
    return hfdlMpduDown(1, 23, {hfdlUnnumbered(hfdlPerfData("UA0901", 51.5, -30.25, 40000, 1, 2)), hfdlUnnumbered(hfdlAcarsHfnpdu("N123UA", "H1", '2', text, true))});
}
static HfdlSquitterSpec sq() { HfdlSquitterSpec s; s.gsId = 1; s.frameIndex = 777; s.mask = 0x21; s.other[0] = {7, true, 3}; return s; }

static int countAcars(const HfdlTelemetry& t) { int n = 0; for (const auto& m : t.messages) n += m.acars && m.acarsCrcOk; return n; }
static int countSq(const HfdlTelemetry& t) { int n = 0; for (const auto& m : t.messages) n += m.squitter && m.gsId == 1 && m.frameIndex == 777; return n; }

// one squitter (600 bit/s) and one data burst of rate m1 on channel 0
static void caseRate(const char* name, int m1, double snr, double cfo, double tOff, const std::vector<HfdlPath>& paths, uint32_t seed = 1) {
    std::vector<Tx> tx = {{0, 1, hfdlSpdu(sq()), 0.3 + tOff, cfo}, {0, m1, acarsPdu(m1), 3.0 + tOff, cfo}};
    const double secs = 3.0 + hfdlBurstSeconds(m1) + 1.0;
    const HfdlTelemetry t = play(tx, snr, paths, secs, seed);
    const int a = countAcars(t), s = countSq(t);
    double pos = 0; for (const auto& m : t.messages) if (m.hasPos) pos = m.lat;
    printf("  %-34s m1 %d %4d bit/s: squitter %d, ACARS %d, SNR %.1f dB, cfo %+.1f Hz, bad %llu\n", name, m1, hfdlRate(m1).bps, s, a, t.snrDb, t.cfoHz, (unsigned long long)t.blocksBad);
    CHECK(s == 1 && a == 1, "%s m1 %d: squitter %d acars %d", name, m1, s, a);
    if (m1 != 0 && m1 != 4) CHECK(std::fabs(pos - 51.5) < 0.01, "%s m1 %d position %f", name, m1, pos);
    CHECK(std::fabs(t.cfoHz - cfo) < 3, "%s cfo %f", name, t.cfoHz);
}

// a fading channel: success counted over several bursts with their own fading (some are lost in deep fades, as on the air)
static void caseFading(const char* name, int m1, double snr, double cfo, const std::vector<HfdlPath>& paths, int trials, int minOk) {
    int ok = 0;
    for (int i = 0; i < trials; i++) {
        std::vector<Tx> tx = {{0, m1, acarsPdu(m1), 0.3, cfo}};
        const HfdlTelemetry t = play(tx, snr, paths, 0.3 + hfdlBurstSeconds(m1) + 0.6, 100 + (uint32_t)(i * 17 + m1));
        ok += countAcars(t) == 1;
    }
    printf("  %-34s m1 %d %4d bit/s: %d of %d bursts\n", name, m1, hfdlRate(m1).bps, ok, trials);
    CHECK(ok >= minOk, "%s m1 %d: %d of %d", name, m1, ok, trials);
}

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    for (int m = 0; m < 8; m++) caseRate("clean", m, 40, 0, 0, {});
    for (int m = 0; m < 8; m++) caseRate("+100 Hz, timing 0.37", m, 25, 100, 0.37 / kRate * 1000, {});
    for (int m = 0; m < 8; m++) caseRate("-100 Hz, timing 13.6 samples", m, 25, -100, 13.6 / kRate, {});
    caseRate("noise 7 dB (BPSK 600)", 1, 7, 20, 0, {});
    caseRate("noise 4 dB (BPSK 300)", 0, 4, -20, 0, {});
    caseRate("noise 11 dB (QPSK 1200)", 2, 11, 40, 0, {});
    caseRate("noise 16 dB (8PSK 1800)", 3, 16, -40, 0, {});
    // real-world impairments (issue #66): +-200 Hz of tuning error (a radio off by a few ppm, LSB / USB dial confusion is not
    // covered), sample clock error, and noise at 10-15 dB
    for (int m = 0; m < 8; m++) caseRate("+200 Hz", m, 25, 200, 0, {});
    for (int m = 0; m < 8; m++) caseRate("-200 Hz", m, 25, -200, 0, {});
    gPpm = 100;
    for (int m = 0; m < 8; m++) caseRate("clock +100 ppm, +150 Hz", m, 25, 150, 0, {});
    gPpm = -100;
    for (int m = 0; m < 8; m++) caseRate("clock -100 ppm, -150 Hz", m, 25, -150, 0, {});
    gPpm = 0;
    caseRate("noise 10 dB, +180 Hz (BPSK 600)", 1, 10, 180, 0, {});
    caseRate("noise 10 dB, -180 Hz (BPSK 300 x2)", 4, 10, -180, 0, {});
    caseRate("noise 13 dB, +120 Hz (QPSK 1200)", 2, 13, 120, 0, {});
    caseRate("noise 15 dB, -120 Hz (8PSK 1800 x2)", 7, 15, -120, 0, {});
    const std::vector<HfdlPath> good = {{0, 0, 0.1, 0}, {0.0005, 0, 0.1, 0}};     // CCIR good: 0.5 ms, 0.1 Hz
    const std::vector<HfdlPath> mod = {{0, 0, 0.5, 0}, {0.001, 0, 0.5, 0}};       // CCIR moderate: 1 ms, 0.5 Hz
    const std::vector<HfdlPath> poor = {{0, 0, 1.0, 0.2}, {0.002, -1, 1.0, -0.3}};   // CCIR poor-like: 2 ms, 1 Hz
    caseRate("2 paths static 1 ms -3 dB, 20 dB", 3, 20, 70, 0, {{0, 0, 0, 0}, {0.001, -3, 0, 0}});
    caseRate("2 paths static 2.5 ms -1 dB, 20 dB", 2, 20, -70, 0, {{0, 0, 0, 0}, {0.0025, -1, 0, 0}});
    for (int m = 0; m < 8; m++) caseFading("CCIR good (0.5 ms, 0.1 Hz), 25 dB", m, 25, 60, good, 4, 3);
    // over 40 bursts: 300 bit/s 98-100 %, 600 and 1200 bit/s 72-92 %, 1800 bit/s 58-60 % (the noise and fading are the same on every platform)
    for (int m = 0; m < 8; m++) caseFading("CCIR moderate (1 ms, 0.5 Hz), 25 dB", m, 25, -60, mod, 8, m % 4 == 3 ? 4 : m % 4 == 0 ? 7 : 5);
    for (int m : {0, 1, 2, 5}) caseFading("2 ms, 1 Hz, 25 dB", m, 25, 100, poor, 8, m == 5 ? 3 : 4);   // 4.2 s at 1 Hz spread: about half get through
    {   // three channels at once, bursts overlapping in time, different levels
        std::vector<Tx> tx;
        for (int c = 0; c < 3; c++) {
            tx.push_back({c, 1, hfdlSpdu(sq()), 0.2 + 0.3 * c, 30.0 * (c - 1), -3.0 * c});
            tx.push_back({c, 2 + (c % 2), acarsPdu(2 + (c % 2)), 2.9 + 0.2 * c, 30.0 * (c - 1), -3.0 * c});
        }
        const HfdlTelemetry t = play(tx, 25, {}, 7.5);
        int on[3] = {};
        for (const auto& m : t.messages) for (int c = 0; c < 3; c++) if (m.freqHz == kChans[c] && (m.squitter || m.acars)) on[c]++;
        printf("  three channels: %d %d %d (squitter + ACARS each), %zu aircraft, %zu stations\n", on[0], on[1], on[2], t.aircraft.size(), t.stations.size());
        CHECK(on[0] == 2 && on[1] == 2 && on[2] == 2, "multi-channel %d %d %d", on[0], on[1], on[2]);
        CHECK(!t.aircraft.empty() && t.aircraft[0].hasPos, "aircraft with a position");
        CHECK(!t.stations.empty() && t.stations[0].heardDirect && t.stations[0].name == "San Francisco", "station");
    }
    {   // noise alone
        const HfdlTelemetry t = play({}, 10, {}, 20);
        printf("  noise only: %zu messages, %llu bad\n", t.messages.size(), (unsigned long long)t.blocksBad);
        CHECK(t.messages.empty(), "noise gave %zu messages", t.messages.size());
    }
    {   // the traffic generator through the receiver: logons, positions, ACARS both ways, every rate
        HfdlGenOptions o;
        o.centerHz = kCentre;
        o.channelsHz = {kChans[0], kChans[1], kChans[2]};
        o.gsIds = {1, 7, 2};
        o.aircraft = 4;
        o.rateFactor = 4;
        o.log = std::make_shared<HfdlSentLog>();
        SynthConfig sc; sc.snrDb = 25; sc.cfoHz = 50;
        auto syn = makeHfdlSynthEx(o, sc, kRate);
        HfdlReceiver rx;
        rx.configure(kRate); rx.setCenterHz(kCentre); rx.setChannels(o.channelsHz);
        std::vector<cf32> buf(4800);
        for (int i = 0; i < 100 * 10; i++) { syn->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
        HfdlTelemetry t; rx.telemetry(t, ~0ull);
        size_t sent = 0; { std::lock_guard<std::mutex> lk(o.log->mu); for (auto& s : o.log->sent) sent += s.startSec + hfdlBurstSeconds(s.m1) < 99; }
        int rates = 0; for (int r = 0; r < 4; r++) rates += t.byRate[r] > 0;
        int logged = 0; for (const auto& a : t.aircraft) logged += a.loggedOn && a.icao != 0;
        printf("  traffic: %zu bursts sent, %llu good, %llu bad, %llu squitters, %llu ACARS, %llu positions, %zu aircraft (%d logged on), %d rates\n", sent, (unsigned long long)t.blocksOk,
               (unsigned long long)t.blocksBad, (unsigned long long)t.squitters, (unsigned long long)t.acarsMsgs, (unsigned long long)t.positionsTotal, t.aircraft.size(), logged, rates);
        CHECK(t.blocksOk == sent && t.blocksBad == 0, "traffic %llu of %zu", (unsigned long long)t.blocksOk, sent);
        CHECK(rates == 4 && logged >= 3 && t.positionsTotal >= 3 && t.acarsMsgs >= 3, "traffic contents");
    }
    printf("  %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    printf(fails ? "hfdl rx: %d FAILED\n" : "hfdl rx: all passed\n", fails);
    return fails ? 1 : 0;
}
