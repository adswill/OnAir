// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers, LTE receiver against the test signal with real-world faults (tests/common/impair.h): carrier offset +-20 kHz, sample clock
// +-20 ppm, low SNR, multipath, the input starting inside a frame, the radio's own sample rate, every bandwidth and port count, two cells
// on one carrier, noise only. The system information must read back exactly as sent; nothing else (paging) may come out.
#include "dect2/cell/cell_asn.h"
#include "dect2/common/stable_random.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/cell/cell_gen.h"
#include "dect2/cell/cell_lte.h"
#include "../common/impair.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;
using namespace dect2::cell;
#include "../common/test_util.h"

struct Got { bool found = false, mib = false, sib1 = false, sib2 = false, sib3 = false, sib4 = false, sib5 = false, wrong = false; int pci = -1, nRb = 0, ports = 0; double cfo = 0, ppm = 0; int nCells = 0; };

static Got decode(const std::vector<cf32>& x, double fs, const CellTestTruth& tr, double offsetHz = 0, int wantPci = -1) {
    Got g;
    LteSearchOpts o;
    const auto res = lteSearchDecode(x.data(), x.size(), fs, offsetHz, o);
    g.nCells = (int)res.size();
    const int pci = wantPci >= 0 ? wantPci : tr.pci;
    for (const auto& r : res) {
        if (r.found.pci != pci) continue;
        g.found = true; g.pci = r.found.pci; g.cfo = r.found.cfoHz; g.ppm = r.found.clockPpm;
        g.mib = r.mib.ok; g.nRb = r.mib.nRb; g.ports = r.mib.ports;
        for (const auto& m : r.si) {
            LteBcch b;
            if (!lteParseBcch(m.bits, b)) { g.wrong = true; continue; }
            if (b.isSib1) {
                const auto& s = b.sib1;
                if (s.plmns.empty() || s.plmns[0].mcc != tr.mcc || s.plmns[0].mnc != tr.mnc || s.tac != tr.tac || s.cellId != tr.cellId || s.band != tr.band) g.wrong = true;
                else g.sib1 = true;
            }
            if (b.has2) g.sib2 = true;
            if (b.has3) g.sib3 = true;
            if (b.has4) { g.sib4 = b.sib4.neigh.size() == tr.intraPci.size(); for (size_t i = 0; g.sib4 && i < tr.intraPci.size(); i++) if (b.sib4.neigh[i].pci != tr.intraPci[i]) g.wrong = true; }
            if (b.has5) { g.sib5 = b.sib5.freqs.size() == tr.interEarfcn.size(); for (size_t i = 0; g.sib5 && i < tr.interEarfcn.size(); i++) if (b.sib5.freqs[i].earfcn != tr.interEarfcn[i]) g.wrong = true; }
        }
    }
    return g;
}

static std::vector<cf32> gen(int scene, CellTestTruth& tr, int frames = 20, int sfn0 = 60, int pci = -1) {
    LteGenCfg g = cellTestCell(scene, &tr, pci);
    g.sfn0 = sfn0;
    return lteGenerate(g, frames);
}

static const char* yn(bool b) { return b ? "y" : "-"; }
static void show(const char* name, const Got& g, double ms) {
    printf("  %-34s pci %d mib %s %d RB %d ports, SIB1 %s 2 %s 3 %s 4 %s 5 %s%s, cfo %+.0f Hz, clock %+.1f ppm (%.0f ms)\n", name, g.pci, yn(g.mib), g.nRb, g.ports,
           yn(g.sib1), yn(g.sib2), yn(g.sib3), yn(g.sib4), yn(g.sib5), g.wrong ? " WRONG" : "", g.cfo, g.ppm, ms);
}

#define RUN(name, call) ([&] { const auto t0 = std::chrono::steady_clock::now(); Got g_ = call; show(name, g_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); return g_; }())

int main(int argc, char** argv) {
    const bool quick = argc > 1 && !strcmp(argv[1], "--quick");
    CellTestTruth tr;
    const std::vector<cf32> base = gen(0, tr);
    const double fs = lteRate(lteNfft(50));
    auto full = [&](const Got& g, const char* what) {
        CHECK(g.found && g.mib && g.nRb == tr.nRb && g.ports == tr.ports, "%s: cell %d mib %d %d RB %d ports", what, g.pci, g.mib, g.nRb, g.ports);
        CHECK(g.sib1 && g.sib2 && g.sib3 && g.sib4 && g.sib5 && !g.wrong, "%s: SIBs %d %d %d %d %d wrong %d", what, g.sib1, g.sib2, g.sib3, g.sib4, g.sib5, g.wrong);
    };
    { const Got g = RUN("clean 10 MHz", decode(base, fs, tr)); full(g, "clean"); CHECK(std::fabs(g.cfo) < 50 && std::fabs(g.ppm) < 1, "clean offsets %.0f Hz %.2f ppm", g.cfo, g.ppm); }
    for (double hz : {20000.0, -20000.0}) {
        std::vector<cf32> x = base;
        impair::shift(x, hz, fs);
        impair::noise(x, 20, 3);
        char nm[64]; snprintf(nm, sizeof nm, "carrier offset %+.0f kHz", hz / 1e3);
        const Got g = RUN(nm, decode(x, fs, tr));
        full(g, nm);
        CHECK(std::fabs(g.cfo - hz) < 100, "%s: measured %.0f Hz", nm, g.cfo);
    }
    for (double ppm : {20.0, -20.0}) {
        std::vector<cf32> x = impair::clock(base, ppm);
        impair::shift(x, -ppm * 806e6 * 1e-6, fs);   // a radio's clock moves its tuning too
        impair::noise(x, 20, 4);
        char nm[64]; snprintf(nm, sizeof nm, "clock %+.0f ppm", ppm);
        const Got g = RUN(nm, decode(x, fs, tr));
        full(g, nm);
        CHECK(std::fabs(g.ppm - ppm) < 3, "%s: measured %.1f ppm", nm, g.ppm);
    }
    {
        std::vector<cf32> x = base;
        impair::echo(x, 15, -6, 1.1);    // 1 us
        impair::echo(x, 61, -12, 2.5);   // 4 us
        impair::noise(x, 18, 5);
        full(RUN("multipath 1 us -6 dB, 4 us -12 dB", decode(x, fs, tr)), "multipath");
    }
    {
        std::vector<cf32> x(base.begin() + 37123, base.end());   // starts inside subframe 2
        impair::shift(x, 7300, fs);
        impair::noise(x, 20, 6);
        const Got g = RUN("starting mid-frame", decode(x, fs, tr));
        CHECK(g.mib && g.sib1 && !g.wrong, "mid-frame: mib %d sib1 %d", g.mib, g.sib1);
    }
    {
        std::vector<cf32> x = base;
        impair::noise(x, 3, 7);
        const Got g = RUN("SNR 3 dB", decode(x, fs, tr));
        CHECK(g.mib && g.sib1 && !g.wrong, "3 dB: mib %d sib1 %d wrong %d", g.mib, g.sib1, g.wrong);
    }
    {
        std::vector<cf32> x = base;
        impair::noise(x, -3, 8);
        const Got g = RUN("SNR -3 dB", decode(x, fs, tr));
        CHECK(g.mib && !g.wrong, "-3 dB: mib %d wrong %d", g.mib, g.wrong);
    }
    {   // the radio's own rate (20 Msps, the HackRF Pro's exact one) with the carrier 2.3 MHz off centre, offset and clock together
        std::vector<cf32> x = impair::clock(base, 12);
        impair::shift(x, 4100, fs);
        std::vector<cf32> y = resampleBlock(x.data(), x.size(), fs, 20e6, 0, fs * 0.95, 0, (size_t)(x.size() * 20e6 / fs));
        impair::shift(y, 2.3e6, 20e6);
        impair::noise(y, 16, 9);
        full(RUN("20 Msps, 2.3 MHz off centre", decode(y, 20e6, tr, 2.3e6)), "20 Msps");
    }
    if (!quick) {
        struct Sc { int scene; const char* name; };
        for (Sc s : {Sc{1, "5 MHz, 4 ports"}, Sc{2, "1.4 MHz, 1 port"}, Sc{3, "20 MHz, 2 ports"}}) {
            CellTestTruth t2;
            std::vector<cf32> x = gen(s.scene, t2);
            const double f2 = lteRate(lteNfft(t2.nRb));
            impair::shift(x, -11000, f2);
            impair::noise(x, 18, 10);
            const Got g = RUN(s.name, decode(x, f2, t2));
            CHECK(g.mib && g.nRb == t2.nRb && g.ports == t2.ports && g.sib1 && g.sib2 && g.sib4 && g.sib5 && !g.wrong, "%s: mib %d %d RB %d ports sib1 %d 2 %d 4 %d 5 %d wrong %d",
                  s.name, g.mib, g.nRb, g.ports, g.sib1, g.sib2, g.sib4, g.sib5, g.wrong);
        }
        {   // two cells of one site on the carrier, 3 dB apart, synchronised (the usual network)
            CellTestTruth t2;
            std::vector<cf32> b = gen(0, t2, 20, 60, 200);
            std::vector<cf32> x = base;
            const float a = std::pow(10.f, -3.f / 20);
            for (size_t i = 0; i < x.size() && i < b.size(); i++) x[i] += a * b[i];
            impair::noise(x, 20, 11);
            const Got g = RUN("two cells, 3 dB apart (strong)", decode(x, fs, tr));
            CHECK(g.mib && g.sib1 && !g.wrong && g.nCells >= 2, "two cells: strong mib %d sib1 %d, %d cells", g.mib, g.sib1, g.nCells);
            const Got w = RUN("two cells, 3 dB apart (weak)", decode(x, fs, t2, 0, 200));
            CHECK(w.mib && !w.wrong, "two cells: weak mib %d wrong %d", w.mib, w.wrong);
        }
    }
    {   // noise only, and a block too short to hold a frame
        std::vector<cf32> x(base.size());
        std::mt19937 rng(12);
        dect2::normal_distribution<float> nd(0.f, 0.1f);
        for (auto& v : x) v = cf32{nd(rng), nd(rng)};
        LteSearchOpts o;
        CHECK(lteSearchDecode(x.data(), x.size(), fs, 0, o).empty(), "noise only gave a cell");
        CHECK(lteSearchDecode(base.data(), 20000, fs, 0, o).empty(), "a short block");
    }
    printf(fails ? "cell rx: %d FAILED\n" : "cell rx: all passed\n", fails);
    return fails ? 1 : 0;
}
