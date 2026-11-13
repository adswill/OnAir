// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CER vs SNR vs fist: the benchmark behind docs/cw.md. Not a ctest: build by hand (see docs/cw.md), run
//   cw_fist_bench [seeds=3] [chars=260] [legacy|adaptive|both]
#include "fist_harness.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cw = dect2::cw;
static void envTune() {
    auto f = [](const char* n, float& v) { if (const char* e = getenv(n)) v = (float)atof(e); };
    cw::ChainTune& c = cw::chainTune();
    f("T_SMOOTH", c.smoothFrac); f("T_SMIN", c.smoothMin); f("T_SWEAK", c.smoothWeak); f("T_WRATIO", c.weakRatio); f("T_SMAX", c.smoothMax); f("T_ON", c.thrOn); f("T_OFF", c.thrOff);
    f("T_HIDEC", c.hiDecayMs); f("T_ATT", c.attackIdle); f("T_HIDOTS", c.hiDecayDots); f("T_OPEN", c.openRatio); f("T_DEB", c.debounceFrac);
    cw::FistTune& t = cw::fistTune();
    f("T_LM", t.lmWeight); f("T_WW", t.wordWeight); f("T_ALPHAL", t.alphaL); f("T_ALPHAC", t.alphaC); f("T_TAILW", t.tailW); f("T_TAILMUL", t.tailMul); f("T_TAILBASE", t.tailBase);
    if (const char* e = getenv("T_DELAY")) t.delay = atoi(e);
}

int main(int argc, char** argv) {
    envTune();
    if (argc > 1 && !strcmp(argv[1], "dbg")) {   // dbg <profile> <snr> <seed> <adaptive 0|1> [chars]
        const cw::FistProfile* p = cw::fistProfile(argv[2]);
        fh::Opt o; o.r.snrDb = atof(argv[3]); o.adaptive = atoi(argv[5]) != 0; o.chars = argc > 6 ? atoi(argv[6]) : 200; o.freeText = getenv("FREE") != nullptr;
        const fh::Result r = fh::run(*p, o, (uint32_t)atoi(argv[4]));
        printf("REF: %s\nDEC: %s\nCONF:%s\nerrs %d / %d  wpm %.1f ratio %.2f\n", r.ref.c_str(), r.dec.c_str(), r.conf.c_str(), r.errs, r.len, r.wpm, r.ratio);
        printf("dot %.0f ms  ratio %.2f  gaps(dots) intra %.2f char %.2f word %.2f  sig d %.2f D %.2f c %.2f w %.2f  fit %.2f q %.2f\n", r.st.dotMs, r.st.dashRatio, r.st.intraGapDots, r.st.charGapDots, r.st.wordGapDots, r.st.sigDot, r.st.sigDash, r.st.sigChar, r.st.sigWord, r.st.fit, r.st.quality);
        return 0;
    }
    const int seeds = argc > 1 ? atoi(argv[1]) : 3;
    const int chars = argc > 2 ? atoi(argv[2]) : 260;
    const char* which = argc > 3 ? argv[3] : "both";
    if (argc > 1 && !strcmp(argv[1], "stress")) {   // stress [seeds]: impairments on the straight key, legacy | adaptive CER %
        const int sd = argc > 2 ? atoi(argv[2]) : 4;
        struct Case { const char* name; const char* prof; void (*set)(fh::Opt&); };
        const Case cases[] = {
            {"clean 15 dB", "straight", [](fh::Opt& o) { o.r.snrDb = 15; }},
            {"QSB 15 dB deep, 6 s", "straight", [](fh::Opt& o) { o.r.snrDb = 25; o.r.qsbDepthDb = 15; o.r.qsbPeriodSec = 6; }},
            {"QSB 25 dB deep, 9 s", "straight", [](fh::Opt& o) { o.r.snrDb = 30; o.r.qsbDepthDb = 25; o.r.qsbPeriodSec = 9; }},
            {"random fading 12 dB", "straight", [](fh::Opt& o) { o.r.snrDb = 25; o.r.qsbDepthDb = 12; o.r.qsbPeriodSec = 5; o.r.qsbRandom = 0.8; }},
            {"key clicks", "straight", [](fh::Opt& o) { o.r.snrDb = 15; o.r.clickDb = -10; }},
            {"QRM -6 dB at 120 Hz", "straight", [](fh::Opt& o) { o.r.snrDb = 20; o.r.qrmDb = -6; o.r.qrmOffsetHz = 120; o.qrm = true; }},
            {"QRM -3 dB at 150 Hz", "bug", [](fh::Opt& o) { o.r.snrDb = 20; o.r.qrmDb = -3; o.r.qrmOffsetHz = 150; o.qrm = true; }},
            {"noise crashes 20/min", "straight", [](fh::Opt& o) { o.r.snrDb = 15; o.r.crashPerMin = 20; }},
            {"carrier 30 Hz off centre", "straight", [](fh::Opt& o) { o.r.snrDb = 15; o.r.offsetHz = 30; }},
            {"speed 12 -> 24 WPM, abrupt", "straight", [](fh::Opt& o) { o.r.snrDb = 15; o.wpm = 12; o.wpm2 = 24; }},
            {"speed 25 -> 10 WPM, abrupt", "straight", [](fh::Opt& o) { o.r.snrDb = 15; o.wpm = 25; o.wpm2 = 10; }},
            {"speed 18 -> 36 WPM, bug", "bug", [](fh::Opt& o) { o.r.snrDb = 15; o.wpm = 18; o.wpm2 = 36; }},
        };
        printf("stress (straight key unless noted), %d seeds: CER %%  legacy | adaptive\n", sd);
        for (const Case& c : cases) {
            double r2[2];
            for (int ad = 0; ad < 2; ad++) {
                int e = 0, l = 0;
                for (int s = 1; s <= sd; s++) {
                    fh::Opt o; o.adaptive = ad; o.chars = 220; c.set(o);
                    fh::Result r = fh::run(*cw::fistProfile(c.prof), o, (uint32_t)s);
                    e += r.errs; l += r.len;
                }
                r2[ad] = 100.0 * e / std::max(1, l);
            }
            printf("  %-30s %6.1f | %6.1f\n", c.name, r2[0], r2[1]);
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "noise")) {   // noise [minutes]: characters decoded from noise alone
        const double min = argc > 2 ? atof(argv[2]) : 10;
        for (int ad = 0; ad < 2; ad++) {
            int n = 0;
            for (uint32_t s = 1; s <= 4; s++) n += fh::noiseChars(ad, min * 60, s);
            printf("%s: %d characters from 4 x %.0f minutes of noise\n", ad ? "adaptive" : "legacy", n, min);
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "score")) {   // score [seeds]: one line, adaptive, mean CER of the profiles at 6/10/15 dB, normal | free text
        const int sd = argc > 2 ? atoi(argv[2]) : 3;
        double m[2] = {0, 0};
        for (int free = 0; free < 2; free++) {
            int cnt = 0;
            for (const cw::FistProfile& p : cw::fistProfiles())
                for (double snr : {6.0, 10.0, 15.0}) {
                    fh::Opt o; o.chars = 200; o.r.snrDb = snr; o.freeText = free;
                    int e = 0, l = 0;
                    for (int s = 1; s <= sd; s++) { const fh::Result r = fh::run(p, o, (uint32_t)s); e += r.errs; l += r.len; }
                    m[free] += 100.0 * e / std::max(1, l); cnt++;
                }
            m[free] /= cnt;
        }
        printf("normal %.2f  free %.2f  sum %.2f\n", m[0], m[1], m[0] + m[1]);
        return 0;
    }
    const double snrs[] = {2, 6, 10, 15, 25};
    for (int pass = 0; pass < 2; pass++) {
        const bool ad = pass == 1;
        if ((ad && !strcmp(which, "legacy")) || (!ad && !strcmp(which, "adaptive"))) continue;
        printf("\n%s decoder: CER %% (SNR in 500 Hz:", ad ? "adaptive" : "legacy");
        for (double s : snrs) printf(" %g", s);
        printf(" dB), %d seeds x %d chars\n", seeds, chars);
        double tot[5] = {0, 0, 0, 0, 0};
        for (const cw::FistProfile& p : cw::fistProfiles()) {
            printf("  %-10s", p.name);
            for (int k = 0; k < 5; k++) {
                fh::Opt o; o.adaptive = ad; o.chars = chars; o.freeText = getenv("FREE") != nullptr; o.r.snrDb = snrs[k];
                int e = 0, l = 0;
                for (int s = 1; s <= seeds; s++) { const fh::Result r = fh::run(p, o, (uint32_t)s); e += r.errs; l += r.len; }
                const double c = 100.0 * e / std::max(1, l);
                tot[k] += c;
                printf(" %6.1f", c);
            }
            printf("\n");
        }
        printf("  %-10s", "mean");
        for (int k = 0; k < 5; k++) printf(" %6.1f", tot[k] / (double)cw::fistProfiles().size());
        printf("\n");
        fflush(stdout);
    }
}
