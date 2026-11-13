// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FLARM receiver under real-world impairments: AWGN near the threshold, carrier offsets of +-10 kHz, chip clock errors of +-50 ppm, two transmitters
// colliding, a transmitter clock a few seconds off at a key-time step boundary. Loopback against our own generator (flarm_gen.h), not on-air captures.
#include "dect2/flarm/flarm_gen.h"
#include "dect2/flarm/flarm_rx.h"
#include "dect2/flarm/flarm_tel.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRefLat = 54.70, kRefLon = 25.30;

static FlarmTx mk(double u0, double t, int ch, uint32_t id, double clockAhead = 0) {
    FlarmTx tx;
    tx.timeSec = t; tx.channel = ch;
    tx.keyTime = (uint32_t)(u0 + t + clockAhead);
    flarm::V6& p = tx.pkt;
    p.addr = id; p.aircraftType = 1; p.alt = 900 + (uint32_t)(t * 3);
    flarm::packPosition(kRefLat + (1000 + 25.0 * t) / 111320.0, kRefLon + 0.01, p.lat19, p.lon20);
    flarm::packVelocity(25.0, 0.0, 0.5, p);
    return tx;
}

static FlarmTelemetry decode(const std::vector<cf32>& x, double rate, double u0) {
    FlarmReceiver r;
    r.configure(rate); r.setUnixTime(u0); r.setReference(kRefLat, kRefLon);
    FlarmTelemetry last; uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += 16384) {
        r.feed(x.data() + i, std::min<size_t>(16384, x.size() - i));
        FlarmTelemetry t;
        if (r.telemetry(t, seq)) { seq = t.seq; last = std::move(t); }
    }
    return last;
}

// n packets of one aircraft, 0.12 s apart on alternating channels
static std::vector<FlarmTx> train(double u0, int n, uint32_t id, double cfo = 0, double gainDb = 0, double t0 = 0.05, double clockAhead = 0) {
    std::vector<FlarmTx> v;
    for (int i = 0; i < n; i++) { FlarmTx t = mk(u0, t0 + 0.12 * i, i & 1, id, clockAhead); t.cfoHz = cfo; t.gainDb = gainDb; v.push_back(t); }
    return v;
}

int main() {
    const double u0 = 1800000000.0 - std::fmod(1800000000.0, 64.0) + 20.0;
    const int N = 30;
    // AWGN: decoded fraction against the signal to noise ratio in 200 kHz (a 100 kchip/s GFSK signal needs about 10 dB)
    double rates[2] = {1e6, 2.4e6};
    for (double rate : rates) {
        printf("  %.1f Msps, accepted of %d:", rate / 1e6, N);
        int atThreshold = 0;
        for (double snr : {20.0, 16.0, 13.0, 11.0, 9.0, 7.0}) {
            const FlarmTelemetry t = decode(flarmScene(rate, 0.12 * N + 0.3, train(u0, N, 0xDD1234), snr, 0, 3), rate, u0);
            printf("  %.0f dB: %llu", snr, (unsigned long long)t.blocksOk);
            if (snr == 16.0) atThreshold = (int)t.blocksOk;
            if (snr == 20.0) CHECK(t.blocksOk >= (uint64_t)(N - 2), "%.1f Msps 20 dB: %llu of %d", rate / 1e6, (unsigned long long)t.blocksOk, N);
            CHECK(t.aircraftCount <= 1, "a made-up aircraft appeared (%u) at %.0f dB", t.aircraftCount, snr);
        }
        printf("\n");
        CHECK(atThreshold >= N * 8 / 10, "%.1f Msps 16 dB: %d of %d", rate / 1e6, atThreshold, N);
    }
    // carrier offset +-10 kHz (and a little more), per transmitter
    for (double cfo : {-14000.0, -10000.0, -4000.0, 4000.0, 10000.0, 14000.0}) {
        const FlarmTelemetry t = decode(flarmScene(2e6, 0.12 * N + 0.3, train(u0, N, 0xDD1234, cfo), 22.0, 0, 4), 2e6, u0);
        CHECK(t.blocksOk >= (uint64_t)(N - 2), "cfo %+.0f Hz: %llu of %d", cfo, (unsigned long long)t.blocksOk, N);
    }
    // chip clock error +-50 ppm
    for (double ppm : {-50.0, 50.0}) {
        const FlarmTelemetry t = decode(flarmScene(2e6, 0.12 * N + 0.3, train(u0, N, 0xDD1234), 22.0, ppm, 5), 2e6, u0);
        CHECK(t.blocksOk >= (uint64_t)(N - 2), "clock %+.0f ppm: %llu of %d", ppm, (unsigned long long)t.blocksOk, N);
    }
    // two transmitters at the same time on the same channel: the stronger is received, the weaker may be lost, nothing made up
    {
        std::vector<FlarmTx> v;
        for (int i = 0; i < N; i++) {
            FlarmTx a = mk(u0, 0.05 + 0.12 * i, 0, 0xDD1234), b = mk(u0, 0.05 + 0.12 * i + 0.0015, 0, 0xDD2222);
            a.gainDb = 0; b.gainDb = -14;
            v.push_back(a); v.push_back(b);
        }
        const FlarmTelemetry t = decode(flarmScene(2e6, 0.12 * N + 0.3, v, 28.0, 0, 6), 2e6, u0);
        int strong = 0;
        for (const auto& a : t.aircraft) { CHECK(a.id == 0xDD1234 || a.id == 0xDD2222, "garbage aircraft %06X", a.id); if (a.id == 0xDD1234) strong = (int)a.packets; }
        printf("  collision: strong aircraft %d of %d packets, %u aircraft\n", strong, N, t.aircraftCount);
        CHECK(strong >= N * 7 / 10, "the stronger of two colliding transmitters: %d of %d", strong, N);
        // equal power: perhaps neither, but never a wrong aircraft
        v.clear();
        for (int i = 0; i < N; i++) { v.push_back(mk(u0, 0.05 + 0.12 * i, 0, 0xDD1234)); v.push_back(mk(u0, 0.05 + 0.12 * i + 0.002, 0, 0xDD2222)); }
        const FlarmTelemetry e = decode(flarmScene(2e6, 0.12 * N + 0.3, v, 28.0, 0, 7), 2e6, u0);
        for (const auto& a : e.aircraft) CHECK(a.id == 0xDD1234 || a.id == 0xDD2222, "garbage aircraft %06X in an equal collision", a.id);
    }
    // two transmitters on the two channels at once (no interference between channels), and in separate slots
    {
        std::vector<FlarmTx> v;
        for (int i = 0; i < N; i++) { v.push_back(mk(u0, 0.05 + 0.12 * i, 0, 0xDD1234)); v.push_back(mk(u0, 0.05 + 0.12 * i, 1, 0xDD2222)); }
        const FlarmTelemetry t = decode(flarmScene(2e6, 0.12 * N + 0.3, v, 24.0, 0, 8), 2e6, u0);
        CHECK(t.aircraftCount == 2 && t.blocksOk >= (uint64_t)(2 * N - 6), "both channels at once: %u aircraft, %llu packets", t.aircraftCount, (unsigned long long)t.blocksOk);
    }
    // the receiver clock is a few seconds off at a step boundary of the 64 s key time (candidates: this step and its neighbour)
    for (double ahead : {-4.0, 4.0}) {
        const double ub = 1800000000.0 - std::fmod(1800000000.0, 64.0) + (ahead > 0 ? 62.0 : 2.0);   // 2 s before / after a boundary
        std::vector<FlarmTx> v = train(ub, 12, 0xDD1234, 0, 0, 0.05, ahead);
        const FlarmTelemetry t = decode(flarmScene(2e6, 0.12 * 12 + 0.3, v, 24.0, 0, 9), 2e6, ub);
        CHECK(t.blocksOk >= 10, "transmitter clock %+.0f s off at a boundary: %llu of 12", ahead, (unsigned long long)t.blocksOk);
    }
    // a transmitter clock a minute off cannot be decoded (the key differs): nothing is accepted or invented
    {
        const FlarmTelemetry t = decode(flarmScene(2e6, 0.12 * N + 0.3, train(u0, N, 0xDD1234, 0, 0, 0.05, 300.0), 24.0, 0, 10), 2e6, u0);
        CHECK(t.aircraftCount == 0 || t.blocksOk < 4, "wrong key time: %u aircraft, %llu packets", t.aircraftCount, (unsigned long long)t.blocksOk);
    }
    printf(fails ? "flarm impair: %d FAILED\n" : "flarm impair: all passed\n", fails);
    return fails ? 1 : 0;
}
