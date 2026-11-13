// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow round-trip sweep, 8 and 16 MHz (radio at 10 and 20 Msps): every valid MCS 0 - 9 x S1G_SHORT and S1G_LONG x both guard intervals x three
// impairment sets (see halow_sweep.h). 16 MHz MCS 8 and 9 use two BCC encoders.
#include "halow_sweep.h"
#include <chrono>
using namespace sweep;
#include "../common/test_util.h"

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    int cases = 0;
    for (int w : {W8, W16}) {
        const double rate = recommendedRateHz(w);
        for (int pre : {PreShort, PreLong}) {
            int worst = 100;
            for (int mcs = 0; mcs <= 9; mcs++) {
                if (!mcsValid(w, mcs, 1)) continue;
                for (int sgi = 0; sgi < 2; sgi++) for (size_t si = 0; si < sets().size(); si++) {
                    HalowGenFrame f;
                    f.psdu = payloadFrame(60); f.width = w; f.mcs = mcs; f.pre = pre; f.sgi = sgi;
                    const Result r = run(f, sets()[si], rate, 10);
                    const double need = si == 0 ? 0.9 : 0.7;
                    cases++;
                    CHECK(r.ok >= need * r.sent && r.bad == 0, "%d MHz %s MCS %d sgi %d [%s]: %llu good, %llu bad FCS of %.0f", 1 << w, preambleName(pre), mcs, sgi, sets()[si].name, (unsigned long long)r.ok, (unsigned long long)r.bad, r.sent);
                    worst = std::min(worst, (int)(100 * r.ok / std::max(1.0, r.sent)));
                }
            }
            printf("  %d MHz %s preamble: all valid MCS passed, worst case %d%% of the PPDUs\n", 1 << w, preambleName(pre), worst);
        }
    }
    printf("  %d cases in %.1f s\n", cases, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return testSummary("halow sweep wide");
}
