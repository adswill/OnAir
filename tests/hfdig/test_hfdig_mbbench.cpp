// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Multi-band monitor CPU: noise IQ at MB_RATE (default 32 Msps) for MB_SECS (default 2) seconds, MB_NB bands selected (FT8 only unless MB_ALL),
// process CPU seconds (all threads) per second of signal. The noise block is made once and repeated.
#include "dect2/hfdig/hfdig_multiband.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#ifdef _WIN32
#include <ctime>
#else
#include <sys/resource.h>
#endif
using namespace dect2;
#ifdef _WIN32
static double cpu() { return double(clock()) / CLOCKS_PER_SEC; }
#else
static double cpu() { rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_utime.tv_sec + r.ru_stime.tv_sec + 1e-6 * (r.ru_utime.tv_usec + r.ru_stime.tv_usec); }
#endif
static double env(const char* n, double d) { const char* s = getenv(n); return s ? atof(s) : d; }
int main() {
    const double rate = env("MB_RATE", 32e6), secs = env("MB_SECS", 2), centre = env("MB_CENTRE", 14e6);
    const int nb = (int)env("MB_NB", 4);
    std::vector<MbSel> sel;
    for (int b = 0; b < kMbBands && (int)sel.size() < nb; b++) sel.push_back({b, 0});
    auto main = makeFtxDecoder();
    main->setStartTime(1767225600.0);
    HfdigMultiBand mb(*main);
    mb.configure(rate, centre, sel);
    const MbStatus st = mb.status();
    printf("rate %.1f Msps centre %.2f MHz: %d selected, %d active, %d bands, %d channels\n", rate / 1e6, centre / 1e6, nb, (int)st.active.size(), st.bands, st.channels);
    const size_t blk = (size_t)(rate / 10);
    std::vector<cf32> x(blk);
    std::mt19937 g(1);
    std::normal_distribution<float> nd(0.f, 0.05f);
    for (auto& v : x) v = cf32(nd(g), nd(g));
    const double c0 = cpu();
    for (int i = 0; i < (int)(secs * 10); i++) mb.feed(x.data(), blk);
    const double c1 = cpu();
    main->waitIdle();
    const MbStatus s2 = mb.status();
    printf("CPU %.3f core-s per s of signal (feed only %.3f), load %.2f, dropped %d tooSlow %d\n", (cpu() - c0) / secs, (c1 - c0) / secs, s2.load, s2.dropped, (int)s2.tooSlow);
}
