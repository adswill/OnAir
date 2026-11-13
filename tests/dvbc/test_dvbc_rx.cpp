// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-C receiver against the test transmitter with impairments: every QAM order, carrier offset, clock offset, an echo, noise near the threshold,
// a signal off the centre, a wrong manual symbol rate, spectral inversion, annex C, and a HackRF's front end (its DC offset in the middle of
// the centred channel, and its synthesiser's phase noise out to a few hundred kHz). The transport stream must come out clean, with the PAT.
#include "dect2/dvbc/dvbc.h"
#include "dect2/dvbc/dvbc_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Case {
    const char* name;
    int qam;
    double rs, snr, cfo, ppm;
    double echoDb;
    bool inv, annexC;
    double manualRs;
    int expectQam;     // receiver setting: -1 automatic
    double seconds;
    double dcRel = 0;  // a DC offset, amplitude relative to the signal's rms
    double pnRad = 0;  // phase noise rms (radians), low-passed at 150 kHz
};

static void run(const Case& c) {
    dvbc::SignalConfig sc;
    sc.qam = c.qam; sc.symbolRate = c.rs; sc.snrDb = c.snr; sc.cfoHz = c.cfo; sc.clockPpm = c.ppm; sc.inverted = c.inv;
    sc.rollOff = c.annexC ? 0.13 : 0.15;
    if (c.echoDb > 0) { sc.echoDb = c.echoDb; sc.echoUs = 0.5; }
    sc.sampleRate = 10e6;
    dvbc::Signal sig(sc);
    DvbcReceiver rx;
    rx.configure(10e6);
    rx.setBlocking(true);
    if (getenv("VERB")) rx.setLogCallback([](const std::string& s) { printf("    %s\n", s.c_str()); });
    rx.setAnnexC(c.annexC);
    rx.setSymbolRate(c.manualRs);
    rx.setQam(c.expectQam);
    uint64_t pk = 0, bad = 0, pat = 0, firstAt = 0, syncErr = 0;
    double firstSecs = -1;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double secs) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* q = p + 188 * i;
            if (firstSecs < 0) firstSecs = secs;
            pk++;
            if (q[0] != 0x47) syncErr++;
            if (q[1] & 0x80) { if (secs > firstSecs + 0.2) bad++; continue; }
            const int pid = ((q[1] & 0x1F) << 8) | q[2];
            if (pid == 0) pat++;
        }
    });
    (void)firstAt;
    std::vector<cf32> buf(100000);
    const size_t total = (size_t)(c.seconds * 10e6);
    double rms = 0, ph = 0;
    uint32_t seed = 12345;
    auto gauss = [&] {   // Box-Muller from a small LCG
        seed = seed * 1664525u + 1013904223u; const double u1 = (seed + 1.0) / 4294967297.0;
        seed = seed * 1664525u + 1013904223u; const double u2 = (seed + 1.0) / 4294967297.0;
        return std::sqrt(-2 * std::log(u1)) * std::cos(2 * 3.14159265358979 * u2);
    };
    const double pa = std::exp(-2 * 3.14159265358979 * 150e3 / 10e6), pb = c.pnRad * std::sqrt(1 - pa * pa);
    for (size_t done = 0; done < total; done += buf.size()) {
        sig.generate(buf.data(), buf.size());
        if (rms == 0) { for (auto& z : buf) rms += std::norm(z); rms = std::sqrt(rms / (double)buf.size()); }
        if (c.pnRad > 0) for (auto& z : buf) { ph = pa * ph + pb * gauss(); z *= cf32((float)std::cos(ph), (float)std::sin(ph)); }
        if (c.dcRel > 0) for (auto& z : buf) z += cf32((float)(0.8 * c.dcRel * rms), (float)(-0.6 * c.dcRel * rms));
        rx.feed(buf.data(), buf.size());
    }
    rx.flush();
    DvbcTelemetry t;
    rx.telemetry(t, 0);
    printf("  %-28s %s  lock %.2f s  pk %llu bad %llu pat %llu  [%s] %s\n", c.name, dvbcSummary(t).c_str(), firstSecs, (unsigned long long)pk, (unsigned long long)bad,
           (unsigned long long)pat, t.stage.c_str(), t.note.c_str());
    if (getenv("PSD")) for (size_t k = 0; k < t.psdDb.size(); k += 32) printf("%zu:%.0f ", k, t.psdDb[k]); printf("\n");
    CHECK(t.tsLock && t.qam == c.qam, "%s: lock %d qam %d", c.name, (int)t.tsLock, t.qam);
    CHECK(pk > 1000 && syncErr == 0, "%s: %llu packets, %llu sync errors", c.name, (unsigned long long)pk, (unsigned long long)syncErr);
    CHECK(bad == 0, "%s: %llu packets with errors after the lock", c.name, (unsigned long long)bad);
    CHECK(pat > 0, "%s: no PAT", c.name);
    CHECK(t.inverted == c.inv, "%s: inversion", c.name);
    CHECK(std::fabs(t.symbolRate / (c.rs * (1 + c.ppm * 1e-6)) - 1) < 3e-5, "%s: symbol rate %.1f", c.name, t.symbolRate);
    CHECK(std::fabs(t.cfoHz - c.cfo) < 300, "%s: cfo %.0f", c.name, t.cfoHz);
}

static void mappingChecks() {
    for (int q = 0; q < dvbc::kNumQam; q++) {
        const dvbc::Constellation& c = dvbc::constellation(q);
        int bad = 0, rot = 0;
        double e = 0;
        for (int k = 0; k < c.order; k++) {
            bad += c.decide(c.pts[(size_t)k]) != k;
            e += std::norm(c.pts[(size_t)k]);
            // a quarter turn keeps the LSBs and moves the quadrant on by one
            const int r = c.decide(c.pts[(size_t)k] * cf32(0, 1));
            const int m = c.bits - 2;
            rot += (r & ((1 << m) - 1)) != (k & ((1 << m) - 1)) || dvbc::quadOf(r >> m) != ((dvbc::quadOf(k >> m) + 1) & 3);
        }
        CHECK(bad == 0 && rot == 0 && std::fabs(e / c.order - 1) < 1e-3, "%s: %d wrong decisions, %d wrong turns, energy %.3f", dvbc::qamName(q), bad, rot, e / c.order);
        // bytes -> symbols -> bits, with a constant quarter turn on the way
        std::vector<uint8_t> by(3 * 400);
        for (size_t i = 0; i < by.size(); i++) by[i] = (uint8_t)(i * 37 + 11);
        dvbc::Mapper mp(q);
        std::vector<cf32> sy;
        mp.push(by.data(), by.size(), sy);
        dvbc::Demapper dm; dm.setQam(q);
        std::vector<uint8_t> bits;
        for (auto z : sy) dm.push(c.decide(z * cf32(-1, 0)), bits);
        int errs = 0;
        for (size_t i = 8; i + 8 <= bits.size() && i / 8 < by.size(); i++) errs += bits[i] != ((by[i / 8] >> (7 - i % 8)) & 1);
        CHECK(errs == 0, "%s: %d bit errors through mapper and demapper", dvbc::qamName(q), errs);
    }
}

// A gap in the samples (a radio that loses a few dozen samples now and then) shifts the byte stream; the receiver must find the frame again
// without giving the lock up: no "lost the stream", and most packets still come out clean.
static void gapCheck(int qam) {
    dvbc::SignalConfig sc;
    sc.qam = qam; sc.symbolRate = 6.875e6; sc.snrDb = 36; sc.sampleRate = 10e6;
    dvbc::Signal sig(sc);
    DvbcReceiver rx;
    rx.configure(10e6);
    rx.setBlocking(true);
    int lost = 0;
    rx.setLogCallback([&](const std::string& m) { if (m.find("lost the stream") != std::string::npos) lost++; });
    uint64_t pk = 0, ok = 0;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) { for (size_t i = 0; i < n; i++) { pk++; ok += !(p[188 * i + 1] & 0x80); } });
    std::vector<cf32> buf(32768);
    for (size_t k = 0; k < 150; k++) {
        sig.generate(buf.data(), buf.size());
        if (k > 20 && k % 8 == 0) rx.feed(buf.data() + 41, buf.size() - 41); else rx.feed(buf.data(), buf.size());   // 41 samples lost every 8th buffer
    }
    rx.flush();
    printf("  %-28s packets %llu clean %llu lost-stream %d\n", "sample gaps", (unsigned long long)pk, (unsigned long long)ok, lost);
    CHECK(lost == 0, "qam %d: the lock was given up %d times after a sample gap", qam, lost);
    CHECK(ok * 10 > pk * 9 && pk > 5000, "qam %d: only %llu of %llu packets clean around sample gaps", qam, (unsigned long long)ok, (unsigned long long)pk);
}

int main(int argc, char** argv) {
    mappingChecks();
    const Case cases[] = {
        {"16-QAM clean", 0, 6.952e6, 35, 0, 0, 0, false, false, 0, -1, 1.2},
        {"32-QAM clean", 1, 6.952e6, 35, 0, 0, 0, false, false, 0, -1, 1.2},
        {"64-QAM clean", 2, 6.952e6, 40, 0, 0, 0, false, false, 0, -1, 1.2},
        {"128-QAM clean", 3, 6.952e6, 40, 0, 0, 0, false, false, 0, -1, 1.2},
        {"256-QAM clean", 4, 6.952e6, 40, 0, 0, 0, false, false, 0, -1, 1.2},
        {"64-QAM +50 kHz +50 ppm", 2, 6.900e6, 32, 50e3, 50, 0, false, false, 0, -1, 1.5},
        {"256-QAM -50 kHz -50 ppm", 4, 6.875e6, 36, -50e3, -50, 0, false, false, 0, -1, 1.5},
        {"64-QAM echo -10 dB 0.5 us", 2, 6.952e6, 32, 10e3, 20, 10, false, false, 0, -1, 1.5},
        {"256-QAM echo -10 dB (45 dB)", 4, 6.952e6, 45, -5e3, 0, 10, false, false, 0, -1, 1.5},
        {"64-QAM echo strong signal", 2, 6.952e6, 45, 0, 0, 10, false, false, 0, 2, 1.0},
        {"16-QAM near threshold", 0, 6.952e6, 17.5, 20e3, 0, 0, false, false, 0, -1, 1.5},
        {"64-QAM near threshold", 2, 6.952e6, 24.5, 20e3, 0, 0, false, false, 0, -1, 1.5},
        {"256-QAM near threshold", 4, 6.952e6, 31, 20e3, 0, 0, false, false, 0, -1, 1.5},
        {"64-QAM off centre 600 kHz", 2, 6.111e6, 32, 600e3, 0, 0, false, false, 0, -1, 1.5},
        {"64-QAM wrong manual rate", 2, 6.952e6, 32, 0, 0, 0, false, false, 6.875e6, -1, 2.5},
        {"64-QAM manual QAM + rate", 2, 6.900e6, 32, 0, 0, 0, false, false, 6.900e6, 2, 1.2},
        {"64-QAM inverted", 2, 6.952e6, 32, 15e3, 0, 0, true, false, 0, -1, 1.5},
        {"256-QAM annex C 5.361", 4, 5.361e6, 36, 0, 0, 0, false, true, 0, -1, 1.5},
        {"256-QAM DC in the middle", 4, 6.875e6, 36, 1.5e3, -80, 0, false, false, 6.875e6, -1, 1.5, 0.5, 0},
        {"256-QAM HackRF phase noise", 4, 6.875e6, 36, 1.5e3, -80, 0, false, false, 6.875e6, -1, 1.5, 0, 0.035},
    };
    for (const auto& c : cases) if (argc < 2 || strstr(c.name, argv[1])) run(c);
    gapCheck(4); gapCheck(1);
    printf(fails ? "dvbc rx: %d FAILED\n" : "dvbc rx: all passed\n", fails);
    return fails ? 1 : 0;
}
