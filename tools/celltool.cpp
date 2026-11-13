// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers tool: search an IQ recording for LTE cells and print what they broadcast.
//   celltool <file> <rate Hz> <centre Hz> [cs8|cf32]     the whole receiver (spectrum, every carrier, system information)
//   celltool --gen <file.cf32> [scene]                     write the test signal (cellTestCell) at its native rate
// Receive only: it reads a file.
#include "dect2/cell/cell_gen.h"
#include "dect2/cell/cell_lte.h"
#include "dect2/cell/cell_rx.h"
#include "dect2/cell/cell_tel.h"
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
        const int scene = argc > 3 ? atoi(argv[3]) : 0;
        cell::LteGenCfg g = cellTestCell(scene);
        g.sfn0 = 60;
        const auto x = cell::lteGenerate(g, 40);
        FILE* f = fopen(argv[2], "wb");
        if (!f) return 1;
        fwrite(x.data(), sizeof(cf32), x.size(), f);
        fclose(f);
        printf("%zu samples at %.2f Msps\n", x.size(), cell::lteRate(cell::lteNfft(g.cell.nRb)) / 1e6);
        return 0;
    }
    if (argc < 4) { fprintf(stderr, "usage: celltool <file> <rate Hz> <centre Hz> [cs8|cf32]\n"); return 2; }
    const double rate = atof(argv[2]), center = atof(argv[3]);
    const bool cs8 = argc < 5 || !strcmp(argv[4], "cs8");
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    if (const char* po = getenv("CELL_PROBE_OFF")) {   // dev: one search at this offset from the centre, on the first 0.2 s
        const size_t n = (size_t)(rate * 0.2);
        std::vector<int8_t> raw(n * 2);
        std::vector<cf32> x(n);
        const size_t got = cs8 ? fread(raw.data(), 2, n, f) : fread(x.data(), sizeof(cf32), n, f);
        if (cs8) for (size_t i = 0; i < got; i++) x[i] = cf32(raw[2 * i] / 128.f, raw[2 * i + 1] / 128.f);
        cell::LteSearchOpts o;
        o.maxCfoHz = 60000;
        for (const auto& r : cell::lteSearchDecode(x.data(), got, rate, atof(po), o))
            printf("PCI %d mib %d %d RB %d ports, %zu SI, cfo %.0f\n", r.found.pci, r.mib.ok, r.mib.nRb, r.mib.ports, r.si.size(), r.found.cfoHz);
        return 0;
    }
    CellReceiver rx;
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
    CellTelemetry t;
    rx.telemetry(t, 0);
    printf("\n%s; %llu blocks, %llu SI messages, %llu failed\n", cellSummary(t).c_str(), (unsigned long long)t.blocks, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    for (const auto& c : t.carriers) printf("carrier %.2f MHz, %.2f MHz wide, %.1f dB over the floor\n", c.freqHz / 1e6, c.widthHz / 1e6, c.levelDb);
    for (const auto& c : t.cells) {
        printf("\nPCI %3d  %.1f MHz EARFCN %d band %d  %s %.1f MHz %d ports  RSRP %.1f dBFS RSRQ %.1f dB SINR %.1f dB  cfo %+.0f Hz  seen %llu\n", c.pci, c.freqHz / 1e6, c.earfcn, c.band,
               c.tdd ? "TDD" : "FDD", c.hasMib ? c.nRb * 0.18 : 0.0, c.ports, c.rsrpDb, c.rsrqDb, c.sinrDb, c.cfoHz, (unsigned long long)c.seen);
        if (c.hasSib1) {
            printf("  SIB1: ");
            for (const auto& p : c.plmns) printf("%s ", p.c_str());
            printf("(%s, %s) TAC %u (0x%X) cell %u = eNB %u sector %d%s, band %d, q-RxLevMin %d dBm, SI window %d ms\n", c.op.c_str(), lteCountry(c.mcc), c.tac, c.tac, c.cellId, c.enb, c.sector,
                   c.barred ? " BARRED" : "", c.band, c.qRxLevMinDbm, c.siWindowMs);
            for (const auto& s : c.sched) { printf("    every %d frames:", s.periodRf); for (int v : s.sibs) printf(" SIB%d", v); printf("\n"); }
        }
        if (c.hasSib2) printf("  SIB2: RS power %d dBm, p-b %d, UL EARFCN %d, UL %d RB, %d preambles\n", c.refSignalPowerDbm, c.pb, c.ulEarfcn, c.ulBwRb, c.raPreambles);
        if (c.hasSib3) printf("  SIB3: q-Hyst %d dB, priority %d, s-IntraSearch %d dB, s-NonIntraSearch %d dB\n", c.qHystDb, c.prio, c.sIntraDb, c.sNonIntraDb);
        if (c.hasSib4) { printf("  SIB4:"); for (const auto& n : c.intra) printf(" %d(%+d)", n.pci, n.qOffsetDb); printf("\n"); }
        if (c.hasSib5) for (const auto& q : c.inter) { printf("  SIB5: EARFCN %d (%.1f MHz) %d RB priority %d", q.earfcn, lteEarfcnToHz(q.earfcn) / 1e6, q.bwRb, q.priority); for (const auto& n : q.cells) printf(" %d", n.pci); printf("\n"); }
        printf("  SIBs read:"); for (int s : c.sibsSeen) printf(" %d", s); printf("\n");
    }
    return 0;
}
