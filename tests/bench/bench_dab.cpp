// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB receiver cost: CPU seconds per second of signal for DabReceiver::feed alone (the test signal is generated first, outside the timer),
// at the DAB rate and at a wider input rate, with a hash of the access-unit bytes and the FIB counts to show the output did not change.
// Usage: bench_dab [rateMsps=2.048] [seconds=8] [snrDb=30] [cfoHz=0]
#include "dect2/dab/dab.h"
#include "dect2/dab/dab_gen.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sys/resource.h>
using namespace dect2;
static double cpu() { rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_utime.tv_sec + r.ru_stime.tv_sec + 1e-6 * (r.ru_utime.tv_usec + r.ru_stime.tv_usec); }
int main(int argc, char** argv) {
    const double rate = (argc > 1 ? atof(argv[1]) : 2.048) * 1e6, secs = argc > 2 ? atof(argv[2]) : 8, snr = argc > 3 ? atof(argv[3]) : 30, cfo = argc > 4 ? atof(argv[4]) : 0;
    dabgen::TxConfig tc; tc.utcSeconds = 1700000000;
    SynthConfig sc; sc.snrDb = 200; sc.cfoHz = cfo;
    auto syn = makeDabSynth(tc, sc, rate);
    const size_t N = (size_t)(secs * rate), blk = 32768;
    std::vector<cf32> sig(N);
    for (size_t i = 0; i < N; i += blk) syn->generate(sig.data() + i, std::min(blk, N - i));
    std::mt19937 rng(5); std::normal_distribution<float> g(0.f, 1.f);
    double p = 0; for (size_t i = 0; i < N; i += 7) p += std::norm(sig[i]);
    p /= (double)((N + 6) / 7);
    const float sg = (float)std::sqrt(p / std::pow(10.0, snr / 10) / 2);
    if (snr < 100) for (auto& v : sig) v += cf32(sg * g(rng), sg * g(rng));
    DabReceiver rx; rx.configure(rate); rx.audio().setSilent(true); rx.select(1);
    uint64_t h = 1469598103934665603ull, nb = 0;
    rx.setFrameTap([&](int, const uint8_t* b, int n) { for (int i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; nb += (uint64_t)n; });
    const double t0 = cpu(); double tl = -1;
    for (size_t i = 0; i < N; i += blk) {
        rx.feed(sig.data() + i, std::min(blk, N - i));
        if (tl < 0) { DabTelemetry t; rx.telemetry(t, 0); if (t.fibOk > 0) tl = (double)(i + blk) / rate; }
    }
    const double c = cpu() - t0;
    DabTelemetry t; rx.telemetry(t, 0);
    printf("rate %.3f Msps snr %.0f cfo %.0f: cost %.4f CPU-s/s  fibOk %llu fibBad %llu  firstFib@%.3fs  bytes %llu hash %016llx\n", rate / 1e6, snr, cfo, c / secs, (unsigned long long)t.fibOk, (unsigned long long)t.fibBad, tl, (unsigned long long)nb, (unsigned long long)h);
}
