// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The GNSS interference and spoofing monitor: the simulated sky alone reports "clear"; a CW tone, a swept chirp, pulses and broadband noise added
// after the baseline has been learned are each found and classified; broadband noise through the whole receiver shows as a C/N0 drop next to the
// noise rise; a sudden position jump and equal C/N0 on every satellite raise the spoofing indicators; the CSV export has the history and the log.
#include "dect2/gnss/gnss_interf.h"
#include "dect2/common/stable_random.h"
#include "dect2/gnss/gnss_testkit.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::gnsstest;
#include "../common/test_util.h"

static const double kRate = 4e6, kTwoPi = 2 * 3.14159265358979;

enum Imp { None, Cw, Chirp, Pulses, Noise };
static const char* impName[] = {"clean", "CW tone", "chirp", "pulses", "broadband noise"};

// adds the impairment to b (sample index `done` of the first one) from `from` seconds on
struct Adder {
    Imp imp;
    double from;
    std::mt19937 rng{7};
    dect2::normal_distribution<float> g{0.f, 1.f};
    double ph = 0;
    void operator()(std::vector<cf32>& b, size_t done) {
        for (size_t k = 0; k < b.size(); k++) {
            const double t = (double)(done + k) / kRate;
            if (t < from) continue;
            switch (imp) {
            case Cw: { const double a = kTwoPi * 700e3 * t; b[k] += cf32((float)(0.3 * std::cos(a)), (float)(0.3 * std::sin(a))); break; }
            case Chirp: {   // sawtooth sweep -1.8 .. +1.8 MHz every 100 us
                const double tp = std::fmod(t, 100e-6), f = -1.8e6 + 3.6e6 * tp / 100e-6;
                ph += kTwoPi * f / kRate;
                b[k] += cf32((float)(0.3 * std::cos(ph)), (float)(0.3 * std::sin(ph)));
                break;
            }
            case Pulses: {  // 12 us pulses of a 200 kHz carrier, 1000 a second
                if (std::fmod(t, 1e-3) < 12e-6) { const double a = kTwoPi * 200e3 * t; b[k] += cf32((float)(0.6 * std::cos(a)), (float)(0.6 * std::sin(a))); }
                break;
            }
            case Noise: b[k] += cf32{0.2f * g(rng), 0.2f * g(rng)}; break;
            default: break;
            }
        }
    }
};

// the monitor alone on the simulated sky: 6 s clean (5 s to learn), then the impairment
static void monitorCase(Imp imp) {
    GnssSimConfig cfg;
    GnssSim sim(cfg, kRate);
    GnssInterfMonitor m;
    m.configure(kRate);
    Adder add{imp, 6.0};
    const size_t chunk = 16384, total = (size_t)(14 * kRate);
    std::vector<cf32> b(chunk);
    int alarmSecs = 0;
    unsigned kindsSeen = 0;
    for (size_t done = 0; done < total; done += chunk) {
        sim.generate(b.data(), chunk);
        add(b, done);
        const double before = m.report().history.size();
        m.process(b.data(), chunk, 0, 0);
        if (m.report().history.size() != before && (double)done / kRate > 7.5) {
            alarmSecs += m.report().status != GnssInterfClear;
            kindsSeen |= m.report().kinds;
        }
    }
    const GnssInterference& r = m.report();
    printf("%-16s status %-12s kinds %-26s floor %.1f dBFS/Hz (rise %+.1f), power %.1f dBFS, flatness %.2f, chirp %.2f (%.3f MHz/us), pulses %.0f/s duty %.3f, %zu lines%s\n",
           impName[imp], gnssInterfStatusName(r.status), gnssInterfKindNames(r.kinds).c_str(), r.noiseDb, r.riseDb, r.powerDb, r.flatness, r.chirpScore,
           r.chirpRateHzPerUs / 1e6, r.pulseRateHz, r.pulseDuty, r.spikes.size(), r.reason.empty() ? "" : (", " + r.reason).c_str());
    CHECK(r.valid && !r.learning, "%s: no report", impName[imp]);
    CHECK(r.history.size() >= 13, "%s: %zu history points", impName[imp], r.history.size());
    switch (imp) {
    case None:
        CHECK(r.status == GnssInterfClear && kindsSeen == 0, "clean signal: status %s, kinds %s", gnssInterfStatusName(r.status), gnssInterfKindNames(kindsSeen).c_str());
        CHECK(std::fabs(r.riseDb) < 0.5, "clean signal: rise %.2f dB", r.riseDb);
        break;
    case Cw: {
        CHECK(r.status == GnssInterfAlarm && (r.kinds & GnssKindCw), "CW: status %s, kinds %s", gnssInterfStatusName(r.status), gnssInterfKindNames(r.kinds).c_str());
        CHECK(!r.spikes.empty() && std::fabs(r.spikes[0].offsetHz - 700e3) < 8e3 && r.spikes[0].aboveDb > 20, "CW: spike %.1f kHz %.1f dB", r.spikes.empty() ? 0 : r.spikes[0].offsetHz / 1e3, r.spikes.empty() ? 0 : r.spikes[0].aboveDb);
        CHECK(!(kindsSeen & (GnssKindChirp | GnssKindPulsed)), "CW classified as %s", gnssInterfKindNames(kindsSeen).c_str());
        break;
    }
    case Chirp:
        CHECK(r.status == GnssInterfAlarm && (r.kinds & GnssKindChirp), "chirp: status %s, kinds %s", gnssInterfStatusName(r.status), gnssInterfKindNames(r.kinds).c_str());
        CHECK(std::fabs(r.chirpRateHzPerUs - 3.6e6 / 100.0) < 0.25 * 3.6e6 / 100.0, "chirp: rate %.4f MHz/us (0.036)", r.chirpRateHzPerUs / 1e6);
        CHECK(!(kindsSeen & (GnssKindCw | GnssKindPulsed)), "chirp classified as %s", gnssInterfKindNames(kindsSeen).c_str());
        break;
    case Pulses:
        CHECK(r.status == GnssInterfAlarm && (r.kinds & GnssKindPulsed), "pulses: status %s, kinds %s", gnssInterfStatusName(r.status), gnssInterfKindNames(r.kinds).c_str());
        CHECK(std::fabs(r.pulseRateHz - 1000) < 150, "pulses: %.0f a second (1000)", r.pulseRateHz);
        CHECK(!(kindsSeen & GnssKindChirp), "pulses classified as %s", gnssInterfKindNames(kindsSeen).c_str());
        break;
    case Noise:
        CHECK(r.status == GnssInterfRaised || r.status == GnssInterfAlarm, "noise: status %s", gnssInterfStatusName(r.status));
        CHECK(r.kinds == GnssKindWideband, "noise: kinds %s", gnssInterfKindNames(r.kinds).c_str());
        CHECK(std::fabs(r.riseDb - 4.8) < 1.0, "noise: rise %.1f dB (4.8)", r.riseDb);
        CHECK(r.flatness > 0.9, "noise: flatness %.2f", r.flatness);
        break;
    }
    if (imp != None) CHECK(alarmSecs >= 5, "%s: %d seconds of interference", impName[imp], alarmSecs);
}

// broadband noise through the receiver: the C/N0 of the tracked satellites drops next to the noise rise
static void receiverCase() {
    GnssSimConfig cfg;
    GnssSim sim(cfg, kRate);
    Options o;
    o.rate = kRate; o.secs = 30;
    auto add = std::make_shared<Adder>(Adder{Noise, 18.0});
    o.hook = [add](std::vector<cf32>& b, size_t done) { (*add)(b, done); return b.size(); };
    Run r = run(sim, o);
    const GnssInterference& m = r.tel.interf;
    double before = 0, after = 0;
    int nb = 0, na = 0, clearBefore = 0, ptsBefore = 0;
    for (const auto& p : m.history) {
        if (p.t > 8 && p.t <= 17 && p.cn0 > 0) { before += p.cn0; nb++; }
        if (p.t > 23 && p.cn0 > 0) { after += p.cn0; na++; }
        if (p.t > 6 && p.t <= 18) { ptsBefore++; clearBefore += p.status == GnssInterfClear; }
    }
    if (nb) before /= nb;
    if (na) after /= na;
    printf("receiver: C/N0 %.1f dB-Hz before the noise, %.1f after (drop %.1f, reported %.1f), rise %.1f dB, status %s (%s), %d tracked\n",
           before, after, before - after, m.cn0DropDb, m.riseDb, gnssInterfStatusName(m.status), m.reason.c_str(), r.tel.nTracked);
    CHECK(nb > 5 && na > 3, "C/N0 history %d / %d points", nb, na);
    CHECK(before - after > 3.0, "C/N0 drop %.1f dB", before - after);
    CHECK(m.cn0DropDb > 3.0, "reported drop %.1f dB", m.cn0DropDb);
    CHECK(clearBefore == ptsBefore && ptsBefore > 8, "clear before the noise: %d of %d seconds", clearBefore, ptsBefore);
    CHECK(m.status == GnssInterfAlarm, "status %s with the noise", gnssInterfStatusName(m.status));
    CHECK(!m.spoofing, "spoofing flagged on the simulated sky: %s", m.spoofReason.c_str());
    bool logged = false;
    for (auto& l : r.log) logged |= l.find("Interference monitor: interference") != std::string::npos;
    CHECK(logged, "no interference event in the log");
}

// spoofing indicators from fabricated reports: a steady fix for a minute, then a 2 km jump
static void spoofCase() {
    GnssInterfMonitor m;
    m.configure(kRate);
    m.setClock([] { return 1.8e9; });
    GnssTelemetry t;
    const float cn0s[] = {38, 42, 45, 47, 40, 35, 44};
    for (float c : cn0s) { GnssChannel ch; ch.state = GnssChEphemeris; ch.cn0 = c; t.channels.push_back(ch); }
    double base[3];
    llaToEcef(54.69, 25.28, 120, base);
    bool earlyFlag = false;
    for (int s = 1; s <= 80; s++) {
        t.signalSecs = s;
        t.fix.valid = true; t.fix.fixCount = (uint32_t)s; t.fix.hErrM = 3; t.fix.systemInFix[GnssGps] = true;
        t.fix.clockBiasM[GnssGps] = 1000 + 2.0 * s; t.fix.clockDriftMps = 2.0;
        const double jump = s >= 70 ? 2000 : 0;
        for (int k = 0; k < 3; k++) t.fix.ecef[k] = base[k] + ((s * 7 + k * 3) % 5 - 2) * 0.8 + (k == 0 ? jump : 0);
        m.update(t);
        if (s < 70) earlyFlag |= m.report().spoofing;
    }
    const GnssInterference& r = m.report();
    printf("spoofing: %s (jump %.0f m, %.0f m from the average of %d fixes)\n", r.spoofReason.c_str(), r.lastJumpM, r.distFromAvgM, r.avgFixes);
    CHECK(!earlyFlag, "spoofing flagged before the jump");
    CHECK(r.spoofing && r.posJump && std::fabs(r.lastJumpM - 2000) < 20, "position jump not flagged (%.0f m)", r.lastJumpM);
    CHECK(r.farFromAverage, "not far from the average");
    CHECK(!r.clockJump && !r.cn0Uniform, "clock jump %d, equal C/N0 %d", r.clockJump, r.cn0Uniform);
    // every satellite at the same C/N0
    for (auto& c : t.channels) c.cn0 = 45.f + 0.1f * (float)(&c - &t.channels[0]) * 0.5f;
    t.signalSecs = 81; m.update(t);
    CHECK(m.report().cn0Uniform && m.report().spoofing, "equal C/N0 not flagged (spread %.2f)", m.report().cn0Spread);
    const std::string csv = gnssInterfCsv(m.report());
    CHECK(csv.find("signal_s,utc,noise_dbfs_hz") == 0 && csv.find("position jumped") != std::string::npos && csv.find("2027-01-15T") != std::string::npos,
          "CSV export:\n%s", csv.substr(0, 400).c_str());
}

int main() {
    for (Imp i : {None, Cw, Chirp, Pulses, Noise}) monitorCase(i);
    spoofCase();
    receiverCase();
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
