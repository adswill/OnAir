// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sferics receiver on synthetic scenes: counts, flashes, timing, noise levels, frequency offset, sample rates from 48 kHz to 2.4 Msps, the
// threshold, mains trains at 50 and 60 Hz (alone and under a storm), and an impulse that is too long. Synthetic signals only: nothing here says how
// it behaves on a real storm. There are no frames, so no reserved bits: the "variants" are the sample rate, offset, noise and mains settings.
#include "dect2/sferics/sferics_gen.h"
#include "dect2/sferics/sferics_rx.h"
#include <cmath>
#include <cstdio>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static SfericsTelemetry run(const std::vector<cf32>& x, double rate, double thr = 10, bool mainsFilter = true) {
    SfericsReceiver rx;
    rx.configure(rate);
    rx.setThresholdDb(thr);
    rx.setMainsFilter(mainsFilter);
    SfericsTelemetry t, last;
    uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += 8192) {
        rx.feed(x.data() + i, std::min<size_t>(8192, x.size() - i));
        while (rx.telemetry(t, seq)) { seq = t.seq; last = t; }
    }
    return last;
}

static SfericsScene storm(double snr) {
    SfericsScene s;
    s.seconds = 11;
    const double t[] = {1.0, 2.3, 3.1, 4.6, 5.3, 6.9, 7.6, 8.8};
    for (double v : t) { s.strikeT.push_back(v); s.strikeSnrDb.push_back(snr); }
    return s;
}

int main() {
    const double rates[] = {48e3, 250e3, 1e6, 2e6, 2.4e6};
    for (double r : rates) {
        const SfericsTelemetry t = run(sfericsRender(storm(25), r), r);
        CHECK(t.strikes == 8 && t.flashes == 8 && t.mains == 0, "rate %.0f: strikes %llu flashes %llu mains %llu (want 8 8 0)", r, (unsigned long long)t.strikes, (unsigned long long)t.flashes, (unsigned long long)t.mains);
        CHECK(t.state == 3 && t.dataValid, "rate %.0f: state %d", r, t.state);
    }
    // frequency offset, noise level
    for (double cfo : {-300e3, 150e3}) {
        SfericsScene s = storm(25); s.cfoHz = cfo;
        const SfericsTelemetry t = run(sfericsRender(s, 2e6), 2e6);
        CHECK(t.strikes == 8, "offset %.0f Hz: strikes %llu", cfo, (unsigned long long)t.strikes);
    }
    for (double nz : {0.002, 0.05}) {
        SfericsScene s = storm(20); s.noiseRms = nz;
        const SfericsTelemetry t = run(sfericsRender(s, 1e6), 1e6);
        CHECK(t.strikes == 8, "noise %.3f: strikes %llu (floor %.1f dB)", nz, (unsigned long long)t.strikes, (double)t.floorDb);
        CHECK(std::fabs(t.floorDb - 20 * std::log10(nz)) < 1.5, "noise %.3f: floor %.1f dB, want %.1f", nz, (double)t.floorDb, 20 * std::log10(nz));
    }
    // timing: the recorded times follow the truth within 0.2 ms
    {
        const SfericsScene s = storm(25);
        const SfericsTelemetry t = run(sfericsRender(s, 2e6), 2e6);
        bool ok = t.recent.size() == 8;
        for (size_t i = 0; ok && i < 8; i++) ok = std::fabs(t.recent[7 - i].timeSec - s.strikeT[i]) < 2e-4;
        CHECK(ok, "strike times off by more than 0.2 ms");
    }
    // threshold: a 14 dB strike counts at 10 dB and not at 20 dB
    {
        const SfericsScene s = storm(14);
        const std::vector<cf32> x = sfericsRender(s, 2e6);
        const SfericsTelemetry a = run(x, 2e6, 10), b = run(x, 2e6, 20);
        CHECK(a.strikes >= 7 && b.strikes == 0, "threshold: %llu at 10 dB, %llu at 20 dB", (unsigned long long)a.strikes, (unsigned long long)b.strikes);
    }
    // flashes: three strokes 60 ms apart are one flash
    {
        SfericsScene s; s.seconds = 4;
        for (double v : {1.0, 1.06, 1.12, 2.5}) { s.strikeT.push_back(v); s.strikeSnrDb.push_back(25); }
        const SfericsTelemetry t = run(sfericsRender(s, 1e6), 1e6);
        CHECK(t.strikes == 4 && t.flashes == 2, "flashes: strikes %llu flashes %llu (want 4 2)", (unsigned long long)t.strikes, (unsigned long long)t.flashes);
    }
    // mains trains, 50 and 60 Hz, alone: not counted; with the filter off: counted
    for (double rate : {100.0, 120.0}) {
        SfericsScene s; s.seconds = 8; s.mainsRate = rate; s.mainsSnrDb = 20;
        const std::vector<cf32> x = sfericsRender(s, 1e6);
        const SfericsTelemetry t = run(x, 1e6), u = run(x, 1e6, 10, false);
        CHECK(t.strikes <= 2 && t.mains > 600 && t.mainsActive, "mains %.0f: strikes %llu mains %llu active %d", rate, (unsigned long long)t.strikes, (unsigned long long)t.mains, (int)t.mainsActive);
        CHECK(u.strikes > 600 && u.mains == 0, "mains %.0f, filter off: strikes %llu", rate, (unsigned long long)u.strikes);
    }
    // a storm under a mains train: the strikes stay, the train goes
    {
        SfericsScene s = storm(30); s.mainsRate = 100; s.mainsSnrDb = 14; s.mainsPhase = 0.0037;
        const SfericsTelemetry t = run(sfericsRender(s, 2e6), 2e6);
        CHECK(t.strikes >= 8 && t.strikes <= 11 && t.mains > 600, "storm + mains: strikes %llu mains %llu (want 8 to 11, many mains)", (unsigned long long)t.strikes, (unsigned long long)t.mains);
    }
    // an impulse of 20 ms is not lightning
    {
        SfericsScene s; s.seconds = 4;
        std::vector<cf32> x = sfericsRender(s, 1e6);
        for (size_t i = 1500000; i < 1520000; i++) x[i] += cf32(0.5f, 0.f);
        const SfericsTelemetry t = run(x, 1e6);
        CHECK(t.strikes == 0 && t.tooLong == 1, "long impulse: strikes %llu tooLong %llu", (unsigned long long)t.strikes, (unsigned long long)t.tooLong);
    }
    // noise only: no strikes at the lowest threshold and the lowest rate
    for (double r : {48e3, 2e6}) {
        SfericsScene s; s.seconds = r < 1e6 ? 60 : 12;
        const SfericsTelemetry t = run(sfericsRender(s, r), r, 6);
        CHECK(t.strikes == 0 && t.state == 2, "noise only at %.0f: strikes %llu (first at %.4f s, %.1f dB, %.0f us, floor %.1f)", r, (unsigned long long)t.strikes, t.recent.empty() ? -1.0 : t.recent[0].timeSec, t.recent.empty() ? 0.0 : (double)t.recent[0].peakDb, t.recent.empty() ? 0.0 : (double)t.recent[0].widthUs, (double)t.floorDb);
    }
    // the built-in test signal: a storm at 1.5 strikes/s for 12 s
    {
        SynthConfig c; c.snrDb = 25;
        auto syn = makeSfericsSynth(c, 1e6);
        std::vector<cf32> x(12000000);
        syn->generate(x.data(), x.size());
        const SfericsTelemetry t = run(x, 1e6);
        CHECK(t.strikes >= 8 && t.strikes <= 40 && t.flashes >= 6, "synth storm: strikes %llu flashes %llu", (unsigned long long)t.strikes, (unsigned long long)t.flashes);
    }
    printf(fails ? "sferics rx: %d FAILED\n" : "sferics rx: all passed\n", fails);
    return fails ? 1 : 0;
}
