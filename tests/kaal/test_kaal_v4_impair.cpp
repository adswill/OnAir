// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL frame version 4 under impairments: payload loss by Es/N0 per profile, carrier offset, sample-clock offset, fractional timing,
// flat fading with Doppler, two-ray multipath, phase noise, 1..4 active voice slots with the rest as data, and the iterative /
// decision-directed receiver parts on and off. Prints the tables of docs/KAAL-NEXT.md section 6; --full runs the longer measurement.
#include "kaal_v4_test_common.h"
#include "dect2/kaal/kaal_beacon.h"
#include "dect2/kaal/kaal_tx.h"
#include "dect2/kaal/kaal_rx.h"
#include <chrono>
#include <cstdio>
#include <cstring>

using namespace kv4;
static int fails = 0;
static bool full = false;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Res { double per; int sent; double lock; Score sc; };

static Res measure(Profile p, const Chan& c, int nSf, uint32_t seed, bool iterative = true, bool dd = true, bool eq = true) {
    const Profile prof[kSlots] = {p, p, p, p};
    Sent sent;
    const std::vector<cf> sig = makeSignal(prof, nSf, sent, seed);
    Chan cc = c;
    cc.seed = seed * 31 + 7;
    if (cc.lead == 0) cc.lead = 700 + (int)(seed % 977);
    const Score sc = runRx(channel(sig, cc), sent, 4, iterative, dd, kFs, eq);
    int s = 0, g = 0;
    for (int i = 0; i < kSlots; i++) { s += sc.sent[i]; g += sc.good[i]; }
    return {s ? 1.0 - (double)g / s : 1.0, s, sc.lockS, sc};
}

static void snrSweep() {
    printf("\n== payload loss (PER) by Es/N0, AWGN, cfo +300 Hz, 4 slots of the profile ==\n");
    const int nSf = full ? 400 : 100;
    const Profile ps[] = {kV32R, kV48, kV60, kV80};
    const double es[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    printf("Es/N0 dB:");
    for (double e : es) printf(" %6.0f", e);
    printf("\n");
    for (Profile p : ps) {
        printf("%-9s", profileName(p));
        double at1 = -1;
        double prevE = 0, prevP = 1;
        for (double e : es) {
            Chan c; c.esn0Db = e; c.cfoHz = 300;
            const Res r = measure(p, c, nSf, 100 + (uint32_t)e);
            printf(" %6.3f", r.per);
            if (at1 < 0 && r.per <= 0.01) {   // log-linear interpolation of the 1 % point
                at1 = (prevP <= 0.01 || r.per <= 0) ? e : prevE + (e - prevE) * (std::log10(prevP) + 2) / (std::log10(prevP) - std::log10(std::max(1e-4, r.per)));
                if (e == es[0]) at1 = e;
            }
            prevE = e; prevP = std::max(1e-4, r.per);
        }
        printf("   1%% at %.1f dB\n", at1);
        // quick assertions with margin (measured values in KAAL-NEXT.md)
        const double lim = p == kV32R ? 5.5 : p == kV48 ? 8.5 : p == kV60 ? 10.0 : 12.5;
        CHECK(at1 > 0 && at1 <= lim, "%s 1%% point %.1f dB above %.1f", profileName(p), at1, lim);
    }
}

static void offsets() {
    printf("\n== offsets, V60, Es/N0 14 dB ==\n");
    const int nSf = full ? 200 : 60;
    struct T { const char* name; Chan c; double maxPer; };
    std::vector<T> ts;
    for (double f : {-4000.0, -3000.0, -1500.0, 0.0, 1500.0, 3000.0, 4000.0}) { T t; t.name = "cfo"; t.c.cfoHz = f; t.maxPer = std::fabs(f) <= 3000 ? 0.02 : 1; ts.push_back(t); }
    for (double ppm : {-100.0, -40.0, 40.0, 100.0}) { T t; t.name = "clock ppm"; t.c.ppm = ppm; t.c.cfoHz = 200; t.maxPer = 0.02; ts.push_back(t); }
    for (double d : {0.25, 0.5, 1.5}) { T t; t.name = "delay"; t.c.delay = d; t.maxPer = 0.02; ts.push_back(t); }
    for (auto& t : ts) {
        t.c.esn0Db = 14;
        const Res r = measure(kV60, t.c, nSf, 7);
        printf("%-10s cfo %+6.0f Hz ppm %+5.0f delay %.2f: PER %.3f (%d), lock %.3f s, est cfo %+7.1f ppm %+6.1f\n", t.name, t.c.cfoHz, t.c.ppm,
               t.c.delay, r.per, r.sent, r.lock, r.sc.cfo, r.sc.ppm);
        CHECK(r.per <= t.maxPer, "%s PER %.3f", t.name, r.per);
    }
}

static void fadingEtc() {
    printf("\n== fading, multipath, phase noise (V60 and V48), receiver parts on/off ==\n");
    const int nSf = full ? 400 : 100;
    struct T { const char* name; Chan c; };
    std::vector<T> ts;
    { T t{"AWGN 10 dB", {}}; t.c.esn0Db = 10; ts.push_back(t); }
    { T t{"Rician K6 fd 10 Hz 14 dB", {}}; t.c.esn0Db = 14; t.c.dopplerHz = 10; t.c.rician = true; ts.push_back(t); }
    { T t{"Rician K6 fd 60 Hz 14 dB", {}}; t.c.esn0Db = 14; t.c.dopplerHz = 60; t.c.rician = true; ts.push_back(t); }
    { T t{"Rayleigh fd 10 Hz 20 dB", {}}; t.c.esn0Db = 20; t.c.dopplerHz = 10; ts.push_back(t); }
    { T t{"Rayleigh fd 60 Hz 20 dB", {}}; t.c.esn0Db = 20; t.c.dopplerHz = 60; ts.push_back(t); }
    { T t{"echo -6 dB 1 sym 16 dB", {}}; t.c.esn0Db = 16; t.c.echoDb = -6; t.c.echoSamples = 3; ts.push_back(t); }
    { T t{"echo -3 dB 0.5 sym 16 dB", {}}; t.c.esn0Db = 16; t.c.echoDb = -3; t.c.echoSamples = 1.5; ts.push_back(t); }
    { T t{"echo -10 dB 2 sym 14 dB", {}}; t.c.esn0Db = 14; t.c.echoDb = -10; t.c.echoSamples = 6; ts.push_back(t); }
    { T t{"phase noise 0.01 rad/smp 14 dB", {}}; t.c.esn0Db = 14; t.c.phaseNoise = 0.01; ts.push_back(t); }
    { T t{"phase noise 0.02 rad/smp 16 dB", {}}; t.c.esn0Db = 16; t.c.phaseNoise = 0.02; ts.push_back(t); }
    printf("%-32s %12s %12s %12s %12s %12s\n", "channel", "V60 full rx", "no iterative", "no DD", "no equaliser", "V48 full rx");
    for (auto& t : ts) {
        t.c.cfoHz = 450;
        const Res a = measure(kV60, t.c, nSf, 21), b = measure(kV60, t.c, nSf, 21, false, true), c = measure(kV60, t.c, nSf, 21, true, false);
        const Res d = measure(kV48, t.c, nSf, 21), e = measure(kV60, t.c, nSf, 21, true, true, false);
        printf("%-32s %12.3f %12.3f %12.3f %12.3f %12.3f   (iterative saves %d)\n", t.name, a.per, b.per, c.per, e.per, d.per, a.sc.saves);
        if (t.c.echoDb > -90) CHECK(a.per < 0.05, "%s: PER %.3f with the equaliser", t.name, a.per);
    }
}

// data goodput: many 32-byte texts (one D68 slot each), n voice slots, the rest data
static void goodput() {
    printf("\n== text/data goodput with 0..4 voice slots (V60), 4 s, short messages (32 bytes, one slot each) ==\n");
    const int nSf = 100;
    printf("%-8s", "Es/N0");
    for (int nv = 0; nv <= 4; nv++) printf("   %d voice", nv);
    printf("   (bytes/s of text delivered)\n");
    for (double e : {8.0, 11.0, 14.0, 20.0}) {
        printf("%-8.0f", e);
        for (int nv = 0; nv <= 4; nv++) {
            Profile prof[kSlots];
            for (int s = 0; s < kSlots; s++) prof[s] = s < nv ? kV60 : kIdle;
            FrameTx tx; tx.configure(kFs);
            for (int s = 0; s < kSlots; s++) tx.setSlotProfile(s, prof[s]);
            for (int m = 0; m < 600; m++) { char b[40]; snprintf(b, sizeof b, "msg %04d abcdefghijklmnopqrstuvw", m); tx.queueText(std::string(b, 32)); }
            std::vector<cf> sig;
            uint8_t vb[40] = {};
            for (int n = 0; n < nSf; n++) { for (int s = 0; s < nv; s++) { vb[0] = (uint8_t)n; tx.pushVoice(s, vb); } tx.superframe(sig); }
            Chan c; c.esn0Db = e; c.cfoHz = -250; c.lead = 300; c.seed = 5 + nv;
            const std::vector<cf> y = channel(sig, c);
            FrameRx rx; rx.configure(kFs);
            std::string t;
            size_t bytes = 0;
            for (size_t pos = 0; pos < y.size(); pos += 4800) {   // poll as the engine does (the receiver keeps at most 64 texts)
                rx.feed(y.data() + pos, std::min<size_t>(4800, y.size() - pos));
                while (rx.pollText(t)) bytes += t.size();
            }
            printf("   %7.0f", bytes / (nSf * kSuperframeS));
            if (e >= 14 && nv == 0) CHECK(bytes / (nSf * kSuperframeS) > 2500, "data goodput %zu", bytes);
        }
        printf("\n");
    }
}

// the v3 text path for comparison: the beacon slice (100 characters every 10 s) under the same per-sample noise rules
static void v3Text() {
    printf("\n== v3 text through the beacon slice (before), 16 kbit/s voice + beacon, 40 s ==\n");
    for (double e : {8.0, 14.0, 20.0}) {
        dect2::kaal::VoiceTx v3;
        v3.configure(48000);
        v3.setBitrate(16);
        dect2::kaal::BeaconFields f;
        f.callsign = "LY2ABC"; f.grid = "KO24pq"; f.powerDbm = 37;
        std::string text(100, 'x');
        for (int i = 0; i < 100; i++) text[i] = (char)('a' + i % 26);
        v3.setBeacon(f, text);
        v3.setBeaconMix(true);
        std::vector<int16_t> pcm(16000 * 40);
        for (size_t i = 0; i < pcm.size(); i++) pcm[i] = (int16_t)(6000 * std::sin(i * 0.05));
        v3.pushAudio(pcm.data(), pcm.size());
        v3.flush();
        std::vector<cf> s(v3.available());
        s.resize(v3.popIq(s.data(), s.size()));
        // v3 is 8 ksym/s at 6 samples per symbol: Es/N0 = 6 Ps / N
        Chan c; c.cfoHz = 0;
        std::vector<cf> y = channel(s, c);
        const double ps = powerOf(s), nvar = ps * 6 / std::pow(10.0, e / 10);
        std::mt19937 rng(3);
        std::normal_distribution<double> nd(0, std::sqrt(nvar / 2));
        for (auto& v : y) v += cf((float)nd(rng), (float)nd(rng));
        dect2::kaal::BeaconSliceRx brx;
        brx.configure(48000);
        brx.feed(y.data(), y.size());
        dect2::kaal::BeaconFields g;
        std::string t;
        size_t bytes = 0;
        int frames = 0;
        while (brx.poll(g, t)) { bytes += t.size(); frames++; }
        printf("Es/N0 %4.0f dB (per 8 ksym/s symbol): %d beacon frames, %zu text bytes in %.0f s = %.1f bytes/s\n", e, frames, bytes, s.size() / 48000.0,
               bytes / (s.size() / 48000.0));
    }
}

static void lockTime() {
    printf("\n== lock time (first decoded slot after the signal starts), V60, cfo random +-3 kHz ==\n");
    std::mt19937 rng(77);
    double worst = 0, sum = 0;
    const int N = full ? 40 : 12;
    for (int i = 0; i < N; i++) {
        Chan c; c.esn0Db = 10; c.cfoHz = (double)(rng() % 6000) - 3000; c.delay = (rng() % 100) / 100.0; c.lead = 2000 + (int)(rng() % 20000);
        const Res r = measure(kV60, c, 30, 300 + i);
        const double lt = r.lock - c.lead / 48000.0;
        worst = std::max(worst, lt); sum += lt;
    }
    printf("%d trials: mean %.3f s, worst %.3f s (v3: 1.25 s fast acquisition, about 3.5 s normally)\n", N, sum / N, worst);
    CHECK(worst < 0.5, "lock time %.3f", worst);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) if (!std::strcmp(argv[i], "--full")) full = true;
    const auto t0 = std::chrono::steady_clock::now();
    snrSweep();
    offsets();
    fadingEtc();
    goodput();
    lockTime();
    if (full) v3Text();
    printf("\n%.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("all ok\n");
    return 0;
}
