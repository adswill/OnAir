// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CDR hybrid modes next to loud analogue FM (#275): the FM programme of the hybrid itself far stronger than the digital sidebands, and a loud
// neighbouring FM station 200 / 400 kHz away (a recording at 98.2 MHz sees the neighbours of a crowded band II). Same method as
// test_cdr_realworld.cpp: the generator's clean signal plus the impairment, written independently of the receiver.
#include "dect2/cdr/cdr_gen.h"
#include "dect2/cdr/cdr_rx.h"
#include "../common/impair.h"
#include "../common/jobs.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

static std::vector<cf32> clean(const int* opt, double secs, double rate) {
    SynthConfig sc;
    for (int i = 0; i < 8; i++) sc.modeOpt[i] = opt[i];
    sc.snrDb = 30;
    auto syn = makeCdrSynth(sc, rate);
    std::vector<cf32> x((size_t)(secs * rate));
    syn->generate(x.data(), x.size());
    return x;
}

// an FM station at `offHz` with `relDb` dB more power than the whole signal, 75 kHz deviation, 1 kHz + 2.3 kHz programme
static void addFm(std::vector<cf32>& x, double offHz, double relDb, double rate) {
    double p = 0;
    for (auto& v : x) p += std::norm(v);
    p /= (double)x.size();
    const float a = (float)std::sqrt(p * std::pow(10.0, relDb / 10.0));
    double t1 = 0, t2 = 0, ph = 0, car = 0;
    for (auto& v : x) {
        const double m = 0.6 * std::sin(t1) + 0.4 * std::sin(t2);
        t1 += 2 * M_PI * 1000.0 / rate; t2 += 2 * M_PI * 2300.0 / rate;
        ph += 2 * M_PI * 75000.0 * m / rate;
        car += 2 * M_PI * offHz / rate;
        const double f = ph + car;
        v += cf32(a * (float)std::cos(f), a * (float)std::sin(f));
        if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
        if (car > M_PI) car -= 2 * M_PI; else if (car < -M_PI) car += 2 * M_PI;
    }
}

static void check(const char* what, const std::vector<cf32>& x, double rate) {
    CdrReceiver rx;
    rx.configure(rate);
    for (size_t i = 0; i < x.size(); i += 50000) rx.feed(x.data() + i, std::min<size_t>(50000, x.size() - i));
    CdrTelemetry t;
    rx.telemetry(t, 0);
    jprintf("%-52s snr %5.1f dB, state %d, CFO %+7.1f Hz, SDC %llu/%llu, LDPC %llu ok %llu bad, %zu services\n", what, (double)t.snrDb, t.state, t.cfoHz, (unsigned long long)t.sdcOk, (unsigned long long)t.sdcBad,
            (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.services.size());
    CHECK(t.state == 3 && t.sdcOk >= 1 && t.blocksOk >= 10 && t.blocksBad * 10 <= t.blocksOk && t.services.size() >= 2, "%s", what);
}

int main(int argc, char** argv) {
    static const int hyb[8] = {3, 3, 2, 0, 1, 3, 0, 0};      // 3/9 64QAM + FM
    testjobs::Jobs jobs;
    const double rate = 2.4e6;
    // loud FM relative to the digital signal (the built-in host FM is 10 dB above the digital power; the extra station is added to it)
    auto run = [&](double off, double relDb, double lateSec, bool print) {
        char b[110];
        snprintf(b, sizeof b, "hybrid, FM %+.0f kHz, %.0f dB over the digital signal%s", off / 1e3, relDb, lateSec > 0 ? ", signal appears late" : "");
        auto x = clean(hyb, 14, rate);
        for (size_t i = 0; i < (size_t)(lateSec * rate); i++) x[i] = cf32(0, 0);   // (a weak or late signal: locks after the first seconds)
        addFm(x, off, relDb - 10.0, rate);
        if (print) {
            CdrReceiver rx; rx.configure(rate);
            for (size_t i = 0; i < x.size(); i += 50000) rx.feed(x.data() + i, std::min<size_t>(50000, x.size() - i));
            CdrTelemetry t; rx.telemetry(t, 0);
            jprintf("%-60s snr %5.1f state %d LDPC %llu ok %llu bad\n", b, (double)t.snrDb, t.state, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        } else check(b, x, rate);
    };
    if (argc > 1) {   // any argument: the wide sweep, printed only
        for (double rel : {10.0, 20.0, 30.0, 40.0}) for (double off : {0.0, 300e3, 400e3, -400e3, 600e3, -600e3, 800e3})
            for (double late : {0.0, 2.5}) jobs.add([=] { run(off, rel, late, true); });
    } else {
        jobs.add([=] { run(0, 20, 0, false); });
        for (double off : {400e3, -400e3}) jobs.add([=] { run(off, 20, 0, false); });
        for (double off : {600e3, -600e3, 800e3}) jobs.add([=] { run(off, 40, 0, false); });
        for (double off : {600e3, -800e3}) jobs.add([=] { run(off, 40, 2.5, false); });   // the neighbour is the strongest block while the signal has not come yet
    }
    jobs.run();
    if (fails) { printf("%d check(s) failed\n", fails.load()); return 1; }
    printf("OK\n");
    return 0;
}
