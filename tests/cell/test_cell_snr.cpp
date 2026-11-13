// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LTE cell search threshold: one 5 MHz cell (20 frames, 200 ms) with a carrier offset, white noise at falling SNR (over the 7.68 MHz
// sample band); each level prints whether the cell is found, its MIB and its SIB1 read. Checked: found with MIB and SIB1 down to 0 dB,
// nothing invented. CELL_SNR_LIST="0,-3,-6" CELL_SNR_SEEDS=4: other levels for a measurement (printed only).
#include "dect2/cell/cell_gen.h"
#include "dect2/cell/cell_lte.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace dect2;
using namespace dect2::cell;
#include "../common/test_util.h"

constexpr double kFs = 7.68e6;

int main() {
    std::vector<double> levels = {10, 5, 0};
    bool measure = false;
    if (const char* e = getenv("CELL_SNR_LIST")) {
        levels.clear(); measure = true;
        for (const char* p = e; *p;) { levels.push_back(atof(p)); while (*p && *p != ',') p++; if (*p) p++; }
    }
    const int seeds = getenv("CELL_SNR_SEEDS") ? atoi(getenv("CELL_SNR_SEEDS")) : 1;
    for (double snr : levels) {
        int found = 0, mib = 0, sib = 0, extra = 0;
        for (int sd = 0; sd < seeds; sd++) {
            LteGenCfg g = cellTestCell(1, nullptr, 77);
            g.seed = 11 + (uint32_t)sd;
            std::vector<cf32> x = lteGenerate(g, 20);
            impair::shift(x, 2300.0 + 1700.0 * sd, kFs);
            impair::noise(x, snr, 5 + (uint32_t)sd);
            LteSearchOpts o;
            const auto res = lteSearchDecode(x.data(), x.size(), kFs, 0, o);
            bool f = false, m = false, s = false;
            for (const auto& r : res) {
                if (r.found.pci != 77) { extra++; continue; }
                f = true; m = m || (r.mib.ok && r.mib.nRb == 25);
                for (const auto& si : r.si) if (si.subframe == 5) s = true;
            }
            found += f; mib += m; sib += s;
        }
        printf("  SNR %+5.1f dB: found %d/%d, MIB %d/%d, SIB1 %d/%d, other cells %d\n", snr, found, seeds, mib, seeds, sib, seeds, extra);
        CHECK(extra == 0, "SNR %.1f: %d cells that are not there", snr, extra);
        if (!measure) CHECK(found == seeds && mib == seeds && sib == seeds, "SNR %.1f: found %d MIB %d SIB1 %d", snr, found, mib, sib);
    }
    return testSummary("cell snr");
}
