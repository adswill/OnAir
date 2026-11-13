// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers, LTE-M (eMTC) indication: the MIB's schedulingInfoSIB1-BR-r13 and systemInfoUnchanged-BR-r15 (TS 36.331 6.2.2) come through the
// generator and the cell search for several values, carrier offsets and noise levels, on long (20 frame) and short (6 frame) blocks; the
// cell's SIB1 is still read (the known-signal canceller rebuilds the MIB with these bits). Also the SIB1-BR repetition and transport block
// tables (TS 36.213 Tables 7.1.6-1 and 7.1.7.2.7-1). Simulated signals only.
#include "dect2/cell/cell_gen.h"
#include "dect2/cell/cell_lte.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
using namespace dect2::cell;
#include "../common/test_util.h"

constexpr double kFs = 7.68e6;

int main() {
    // the tables
    CHECK(lteSib1BrReps(0) == 0 && lteSib1BrTbs(0) == 0, "value 0: not sent");
    CHECK(lteSib1BrReps(1) == 4 && lteSib1BrReps(2) == 8 && lteSib1BrReps(3) == 16 && lteSib1BrReps(16) == 4 && lteSib1BrReps(18) == 16, "repetitions");
    CHECK(lteSib1BrTbs(1) == 208 && lteSib1BrTbs(4) == 256 && lteSib1BrTbs(9) == 328 && lteSib1BrTbs(10) == 504 && lteSib1BrTbs(15) == 712 && lteSib1BrTbs(18) == 936, "TBS");
    CHECK(lteSib1BrReps(19) == 0 && lteSib1BrTbs(31) == 0, "reserved values");
    CHECK(lteMibSib1Br((7 << 5) | 16) == 7 && lteMibSiUnchangedBr((7 << 5) | 16) && !lteMibSiUnchangedBr(7 << 5), "field split");

    struct Case { int sib1Br; bool unchanged; double cfo, snr; int frames; };
    const Case cases[] = {
        {0, false, 2300, 10, 20}, {1, false, -4100, 10, 20}, {7, true, 1200, 3, 20}, {18, false, 5200, 0, 20},
        {12, true, -800, 10, 6}, {5, false, 3300, 5, 6},
    };
    int k = 0;
    for (const Case& c : cases) {
        LteGenCfg g = cellTestCell(1, nullptr, 77 + k);
        g.cell.mibTail = (c.sib1Br << 5) | (c.unchanged ? 16 : 0);
        g.seed = 21 + (uint32_t)k;
        std::vector<cf32> x = lteGenerate(g, c.frames);
        impair::shift(x, c.cfo, kFs);
        impair::noise(x, c.snr, 9 + (uint32_t)k);
        LteSearchOpts o;
        const auto res = lteSearchDecode(x.data(), x.size(), kFs, 0, o);
        bool mib = false, tail = false, sib1 = false; int other = 0, got = -1;
        for (const auto& r : res) {
            if (r.found.pci != 77 + k) { other++; continue; }
            if (r.mib.ok) { mib = true; got = r.mib.mibTail; tail = r.mib.mibTail == g.cell.mibTail; }
            for (const auto& si : r.si) if (si.subframe == 5) sib1 = true;
        }
        printf("  SIB1-BR %2d%s, offset %+5.0f Hz, SNR %4.1f dB, %2d frames: MIB %d (tail %d of %d), SIB1 %d\n", c.sib1Br, c.unchanged ? " unchanged" : "", c.cfo, c.snr, c.frames,
               mib, got, g.cell.mibTail, sib1);
        CHECK(other == 0, "case %d: %d cells that are not there", k, other);
        CHECK(mib && tail, "case %d: MIB %d, tail %d (sent %d)", k, mib, got, g.cell.mibTail);
        if (c.frames >= 20) CHECK(sib1, "case %d: SIB1 not read", k);
        k++;
    }
    return testSummary("cell ltem");
}
