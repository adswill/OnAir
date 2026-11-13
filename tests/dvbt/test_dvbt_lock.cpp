// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-T lock time (3.2 quality pass): signal time from the first sample to the first good transport packet, for different carrier modes, guard
// intervals, offsets, noise levels and a start in the middle of a symbol. Prints the table; fails only when a case never locks or takes too long.
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/dvbt/dvbt_rx.h"
#include "dect2/common/stable_random.h"
#include "dect2/t2/t2.h"
#include "../common/jobs.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};

struct Case { const char* name; int mode, gi, mod, cr; double cfoHz, snrDb, leadFrac; double maxSec; };

// signal seconds until the first good packet and until twenty good packets in a row (-1: never)
static void run(const Case& c, double& first, double& steady) {
    dvbt::Params p; p.mode = c.mode; p.guard = c.gi; p.mod = c.mod; p.crHp = c.cr; p.crLp = c.cr;
    const double fn = nativeRateHz(8);
    uint32_t counter = 0;
    dvbt::Generator gen(p, [&](uint8_t* pkt) {
        pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0x00; pkt[3] = 0x10;
        memcpy(pkt + 4, &counter, 4);
        for (int i = 8; i < 188; i++) pkt[i] = (uint8_t)(counter * 31 + i * 7);
        counter++;
    });
    std::mt19937 rng(11);
    dect2::normal_distribution<float> nd(0, 1);
    const float sigma = (float)(std::pow(10.0, -c.snrDb / 20.0) / std::sqrt(2.0));
    const int N = dvbt::fftN(c.mode), G = N / (c.gi == dvbt::kGi4 ? 4 : c.gi == dvbt::kGi8 ? 8 : c.gi == dvbt::kGi16 ? 16 : 32);
    // start inside a symbol: skip the first part of the first symbol
    std::vector<cf32> sig, sym;
    const int symbols = (int)(c.maxSec * fn / (N + G)) + 2;
    for (int s = 0; s < symbols; s++) { gen.nextSymbol(sym); sig.insert(sig.end(), sym.begin(), sym.end()); }
    sig.erase(sig.begin(), sig.begin() + (size_t)(c.leadFrac * (N + G)));
    const double dph = 2 * M_PI * c.cfoHz / fn;
    for (size_t i = 0; i < sig.size(); i++) {
        const double a = dph * (double)i;
        sig[i] = sig[i] * cf32((float)std::cos(a), (float)std::sin(a)) + cf32{nd(rng), nd(rng)} * sigma;
    }
    DvbtReceiver rx;
    rx.configure(fn, 8);
    first = steady = -1;
    size_t fed = 0;
    int run20 = 0;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* q = pk + i * 188;
            uint32_t cn; memcpy(&cn, q + 4, 4);
            bool ok = !(q[1] & 0x80) && q[0] == 0x47 && q[1] == 0x01 && q[2] == 0x00;
            for (int j = 8; j < 188 && ok; j++) ok = q[j] == (uint8_t)(cn * 31 + j * 7);
            if (ok && first < 0) first = (double)fed / fn;
            run20 = ok ? run20 + 1 : 0;
            if (run20 >= 20 && steady < 0) steady = (double)fed / fn;
        }
    });
    const size_t step = 4096;
    for (size_t i = 0; i < sig.size() && steady < 0; i += step) { const size_t n = std::min(step, sig.size() - i); fed = i + n; rx.feed(sig.data() + i, n); }
}

int main(int argc, char** argv) {
    const bool printOnly = argc > 1;
    const Case cases[] = {
        {"8K 1/16 64QAM 3/4 clean", dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, 0, 35, 0.0, 1.5},
        {"8K 1/16 64QAM 3/4 mid-symbol", dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, 0, 35, 0.6, 1.5},
        {"8K 1/16 64QAM 3/4 cfo +30 kHz", dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, 30000, 30, 0.3, 1.5},
        {"8K 1/16 64QAM 3/4 cfo -8.3 kHz", dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, -8300, 30, 0.7, 1.5},
        {"8K 1/32 16QAM 2/3 20 dB", dvbt::k8K, dvbt::kGi32, dvbt::k16Qam, dvbt::kR23, 2000, 20, 0.5, 1.5},
        {"8K 1/4 QPSK 1/2 8 dB", dvbt::k8K, dvbt::kGi4, dvbt::kQpsk, dvbt::kR12, 1500, 8, 0.2, 1.5},
        {"2K 1/8 16QAM 2/3 clean", dvbt::k2K, dvbt::kGi8, dvbt::k16Qam, dvbt::kR23, 0, 35, 0.4, 1.5},
        {"2K 1/4 QPSK 1/2 cfo +5 kHz", dvbt::k2K, dvbt::kGi4, dvbt::kQpsk, dvbt::kR12, 5000, 15, 0.8, 1.5},
    };
    double sum = 0;
    jprintf("%-34s %10s %10s\n", "DVB-T lock", "first ms", "steady ms");
    std::vector<double> f(sizeof cases / sizeof cases[0]), st(f.size());
    testjobs::Jobs jobs;
    for (size_t i = 0; i < f.size(); i++) jobs.add([&, i] { run(cases[i], f[i], st[i]); });
    jobs.run();
    for (size_t i = 0; i < f.size(); i++) {
        jprintf("%-34s %10.0f %10.0f\n", cases[i].name, 1000 * f[i], 1000 * st[i]);
        sum += st[i] < 0 ? 1.5 : st[i];
        if (!printOnly && (f[i] < 0 || f[i] > cases[i].maxSec)) { jprintf("FAIL: %s did not lock in time\n", cases[i].name); fails++; }
    }
    jprintf("mean steady lock %.0f ms\n", 1000 * sum / f.size());
    return fails ? 1 : 0;
}
