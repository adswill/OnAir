// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-C threshold and time-to-lock sweep (not a ctest: run by hand, see docs/SENSITIVITY.md, DVB section).
//   sens_dvbc <qam 0..4 = 16..256> [--snr a:b:step] [--trials n] [--seconds s] [--cfo Hz] [--ppm x] [--echo dB] [--rs symbol rate]
// SNR is Es/N0 of the test transmitter (dvbc::Signal). A trial passes when the stream locks within the first half of the signal and no packet
// is damaged from 200 ms after the first one. Time to lock: seconds of signal to the first packet, the receiver measuring the symbol rate and
// the QAM order itself (as with a channel scan).
#include "dect2/dvbc/dvbc.h"
#include "dect2/dvbc/dvbc_rx.h"
#include "dect2/dvbc/dvbc_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;

struct Trial { bool pass = false; double lock = -1; uint64_t bad = 0, pk = 0; };

static Trial run(int qam, double snr, double secs, double cfo, double ppm, double echo, double rs, uint32_t seed) {
    dvbc::SignalConfig sc;
    sc.qam = qam; sc.symbolRate = rs; sc.snrDb = snr; sc.cfoHz = cfo; sc.clockPpm = ppm; sc.sampleRate = 10e6; sc.seed = seed;
    if (echo > 0) { sc.echoDb = echo; sc.echoUs = 0.5; }
    dvbc::Signal sig(sc);
    DvbcReceiver rx;
    rx.configure(10e6);
    rx.setBlocking(true);
    Trial t;
    size_t fed = 0;   // samples handed over when a packet comes out (blocking: the decoder keeps up with the input)
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
        const double s = (double)fed / 10e6;
        for (size_t i = 0; i < n; i++) {
            if (t.lock < 0) t.lock = s;
            t.pk++;
            if ((p[188 * i + 1] & 0x80) && s > t.lock + 0.2) t.bad++;
        }
    });
    std::vector<cf32> buf(100000);
    const size_t total = (size_t)(secs * 10e6);
    for (size_t done = 0; done < total; done += buf.size()) { sig.generate(buf.data(), buf.size()); fed = done + buf.size(); rx.feed(buf.data(), buf.size()); }
    rx.flush();
    t.pass = t.lock >= 0 && t.lock < 0.5 * secs && t.bad == 0 && t.pk > 100;
    return t;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: see the head of tests/dvbc/sens_dvbc.cpp\n"); return 2; }
    const int qam = atoi(argv[1]);
    double s0 = 20, s1 = 30, ss = 0.5, secs = 1.5, cfo = 0, ppm = 0, echo = 0, rs = 6.952e6;
    int trials = 4;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        auto nx = [&]() { return i + 1 < argc ? argv[++i] : (char*)"0"; };
        if (a == "--snr") sscanf(nx(), "%lf:%lf:%lf", &s0, &s1, &ss);
        else if (a == "--trials") trials = atoi(nx());
        else if (a == "--seconds") secs = atof(nx());
        else if (a == "--cfo") cfo = atof(nx());
        else if (a == "--ppm") ppm = atof(nx());
        else if (a == "--echo") echo = atof(nx());
        else if (a == "--rs") rs = atof(nx());
    }
    double thr = NAN, thrLock = -1;
    for (double snr = s0; snr <= s1 + 1e-9; snr += ss) {
        int pass = 0; std::vector<double> lk; uint64_t bad = 0, pk = 0;
        for (int k = 0; k < trials; k++) {
            const Trial t = run(qam, snr, secs, cfo, ppm, echo, rs, 1000 + (uint32_t)k);
            pass += t.pass; bad += t.bad; pk += t.pk;
            if (t.lock >= 0) lk.push_back(t.lock);
        }
        std::sort(lk.begin(), lk.end());
        const double med = lk.empty() ? -1 : lk[lk.size() / 2], p90 = lk.empty() ? -1 : lk[std::min(lk.size() - 1, (size_t)std::ceil(0.9 * lk.size()) - 1)];
        printf("Es/N0 %5.2f dB: pass %d/%d  lock median %.3f s p90 %.3f s  packets %llu bad %llu\n", snr, pass, trials, med, p90, (unsigned long long)pk, (unsigned long long)bad);
        fflush(stdout);
        if (std::isnan(thr) && pass * 10 >= 9 * trials) { thr = snr; thrLock = med; }
    }
    if (!std::isnan(thr)) printf("THRESHOLD %.2f dB  lock median %.3f s\n", thr, thrLock);
    else printf("THRESHOLD none in range\n");
    return 0;
}
