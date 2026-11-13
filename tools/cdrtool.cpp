// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// cdrtool: run an IQ recording through the CDR receiver and print one line per second (lock, SNR, level, frames, audio). Muted, no sound.
// usage: cdrtool FILE ri8|cf32 RATE [skipSec] [maxSec] [serviceHex] [fadeSpec...]
#include "dect2/cdr/cdr_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace dect2;

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: cdrtool FILE ri8|cf32 RATE [skipSec] [maxSec] [serviceHex]\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    const bool i8 = !strcmp(argv[2], "ri8");
    const double rate = atof(argv[3]);
    const double skip = argc > 4 ? atof(argv[4]) : 0, maxSec = argc > 5 ? atof(argv[5]) : 1e9;
    int listen = argc > 6 ? (int)strtol(argv[6], nullptr, 16) : 0;
    const size_t bps = i8 ? 2 : 8;
    fseek(f, (long)(skip * rate) * (long)bps, SEEK_SET);
    CdrReceiver rx;
    rx.configure(rate);
    rx.setMuted(true);
    rx.setVolume(0);
    double t = 0;
    rx.setLogCallback([&](const std::string& s) { printf("  [%7.2f] %s\n", t, s.c_str()); });
    const size_t N = 50000;
    std::vector<cf32> x(N);
    std::vector<int8_t> b8(N * 2);
    std::vector<float> bf(N * 2);
    double next = 1;
    uint64_t pLocks = 0, pUnl = 0, pIn = 0, pBad = 0, pPcm = 0, pOk = 0, pBadB = 0, pSub = 0;
    size_t total = 0;
    uint64_t lastSeq = 0;
    printf("  sec state lock/unl   snr  level  dCW ok/bad  muxOk/Bad  draUnits(bad)  pcmSec  cfo  sync siOk/Bad drift\n");
    while (total < (size_t)(maxSec * rate)) {
        size_t n;
        if (i8) { n = fread(b8.data(), 2, N, f); for (size_t i = 0; i < n; i++) x[i] = cf32(b8[2 * i] / 128.f, b8[2 * i + 1] / 128.f); }
        else { n = fread(bf.data(), 8, N, f); for (size_t i = 0; i < n; i++) x[i] = cf32(bf[2 * i], bf[2 * i + 1]); }
        if (!n) break;
        rx.feed(x.data(), n);
        total += n;
        t = total / rate;
        CdrTelemetry tl;
        if (rx.telemetry(tl, lastSeq)) {
            lastSeq = tl.seq;
            if (!listen) { for (const auto& s : tl.services) if (s.audio) { listen = s.id; printf("  listening to service %04X\n", listen); break; } }
            if (listen) rx.setListen(listen);
            if (t >= next) {
                printf("%5.0f  %d   %llu/%llu  %5.1f %6.1f  %3llu/%-3llu   %3llu/%-3llu   %4llu(%llu)   %5.2f  %+6.0f  %.2f %llu/%llu %.1f\n", t, tl.state, (unsigned long long)tl.locks, (unsigned long long)tl.unlocks,
                       tl.snrDb, tl.levelDb, (unsigned long long)(tl.blocksOk - pOk), (unsigned long long)(tl.blocksBad - pBadB), (unsigned long long)tl.muxOk, (unsigned long long)tl.muxBad,
                       (unsigned long long)(tl.draUnitsIn - pIn), (unsigned long long)(tl.draUnitsBad - pBad), (tl.draPcmFrames - pPcm) / (double)std::max(1, tl.draRateHz), tl.cfoHz, tl.syncMetric, (unsigned long long)tl.siOk, (unsigned long long)tl.siBad, tl.timingDriftPpm);
                pOk = tl.blocksOk; pBadB = tl.blocksBad; pIn = tl.draUnitsIn; pBad = tl.draUnitsBad; pPcm = tl.draPcmFrames;
                next = std::floor(t) + 1;
            }
        }
    }
    CdrTelemetry tl;
    rx.telemetry(tl, 0);
    printf("total %.1f s: locks %llu unlocks %llu, SDC %llu/%llu, LDPC %llu ok %llu bad, mux %llu/%llu, DRA units %llu (refused %llu), PCM %.1f s\n", total / rate,
           (unsigned long long)tl.locks, (unsigned long long)tl.unlocks, (unsigned long long)tl.sdcOk, (unsigned long long)tl.sdcBad, (unsigned long long)tl.blocksOk,
           (unsigned long long)tl.blocksBad, (unsigned long long)tl.muxOk, (unsigned long long)tl.muxBad, (unsigned long long)tl.draUnitsIn, (unsigned long long)tl.draUnitsBad,
           tl.draPcmFrames / (double)std::max(1, tl.draRateHz));
    return 0;
}
