// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GSM-R tool: search an IQ recording for GSM base stations and print what they broadcast.
//   gsmrtool <file> <rate Hz> <centre Hz> [cs8|cf32]     the whole receiver (spectrum, every carrier, synchronisation, BCCH)
//   gsmrtool --gen <file.cf32> [scene] [seconds]          write the test signal (gsmr_gen.h) at 5 Msps, centred on 922.8 MHz
// Receive only: it reads a file.
#include "dect2/gsmr/gsmr_gen.h"
#include "dect2/gsmr/gsmr_rx.h"
#include "dect2/gsmr/gsmr_tel.h"
#include "dect2/source/source.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "--gen")) {
        SynthConfig sc;
        sc.mode = 41; sc.snrDb = 20; sc.modeOpt[0] = argc > 3 ? atoi(argv[3]) : 0;
        const double sec = argc > 4 ? atof(argv[4]) : 5.0, rate = 5e6;
        auto syn = makeGsmrSynth(sc, rate);
        FILE* f = fopen(argv[2], "wb");
        if (!f) return 1;
        std::vector<cf32> x(1 << 18);
        size_t left = (size_t)(sec * rate);
        while (left) {
            const size_t n = std::min(left, x.size());
            syn->generate(x.data(), n);
            fwrite(x.data(), sizeof(cf32), n, f);
            left -= n;
        }
        fclose(f);
        printf("%.1f s at %.1f Msps, centre %.1f MHz\n", sec, rate / 1e6, kGsmrTestCentreHz / 1e6);
        return 0;
    }
    if (argc < 4) { fprintf(stderr, "usage: gsmrtool <file> <rate Hz> <centre Hz> [cs8|cf32]\n"); return 2; }
    const double rate = atof(argv[2]), center = atof(argv[3]);
    const bool cs8 = argc < 5 || !strcmp(argv[4], "cs8");
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    GsmrReceiver rx;
    rx.configure(rate);
    rx.setCenterHz(center);
    rx.setLogCallback([](const std::string& s) { printf("%s\n", s.c_str()); });
    const size_t chunk = (size_t)(rate * 0.01);
    std::vector<int8_t> raw(chunk * 2);
    std::vector<cf32> x(chunk);
    for (;;) {
        size_t got;
        if (cs8) {
            got = fread(raw.data(), 2, chunk, f);
            for (size_t i = 0; i < got; i++) x[i] = cf32(raw[2 * i] / 128.f, raw[2 * i + 1] / 128.f);
        } else got = fread(x.data(), sizeof(cf32), chunk, f);
        if (got == 0) break;
        rx.feed(x.data(), got);
        while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    fclose(f);
    while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::vector<cf32> z(chunk, cf32(0, 0));
    rx.feed(z.data(), 1);   // publishes
    GsmrTelemetry t;
    rx.telemetry(t, 0);
    printf("\n%s; %llu blocks, %llu SI messages, %llu failed\n", gsmrSummary(t).c_str(), (unsigned long long)t.blocks, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    for (const auto& c : t.carriers) printf("carrier %.3f MHz (ARFCN %d), %.1f dB over the floor, %s\n", c.freqHz / 1e6, gsmrHzToArfcn(c.freqHz), c.levelDb, c.synced ? "synchronised" : "no sync");
    for (const auto& c : t.cells) {
        printf("\nARFCN %d  %.1f MHz%s  BSIC %d-%d  level %.1f dBFS SNR %.1f dB  offset %+.0f Hz  seen %llu  FN %d\n", c.arfcn, c.freqHz / 1e6, c.railway ? " (GSM-R)" : "", c.ncc, c.bcc, c.levelDb,
               c.snrDb, c.cfoHz, (unsigned long long)c.seen, c.fn);
        if (c.hasLai) printf("  LAI %03d-%0*d (%s, %s) LAC %u (0x%X)\n", c.mcc, c.mncDigits, c.mnc, c.op.c_str(), gsmrCountry(c.mcc), c.lac, c.lac);
        if (c.hasSi3) printf("  SI3: cell %u, attach %s, CCCH-CONF %d, AG blocks %d, paging mfrms %d, T3212 %d, RLT %d, hyst %d dB, min level %d, cell bar %d\n", c.cellId, c.attach ? "yes" : "no",
                             c.ccchConf, c.agBlks, c.pagMfrms, c.t3212, c.rlTimeout, c.reselHystDb, c.rxLevAccessMin, c.cellBar);
        if (c.hasSi1) { printf("  SI1 cell allocation:"); for (int n : c.cellAlloc) printf(" %d", n); printf("\n"); }
        if (c.hasSi2) { printf("  SI2 neighbours:"); for (int n : c.neigh) printf(" %d", n); printf("\n"); }
        printf("  messages read: %zu types\n", c.typesSeen.size());
    }
    return 0;
}
