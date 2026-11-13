// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow round-trip sweep, 1 / 2 / 4 MHz: every valid MCS (0 - 9, and 10 at 1 MHz) x both preambles (S1G_1M at 1 MHz, S1G_SHORT and S1G_LONG above)
// x both guard intervals x three impairment sets (clean; carrier offset +30 kHz, signal off the centre, +40 ppm; offset -30 kHz, -40 ppm, a 2 us echo, the
// stream starting inside a PPDU). Each case must give a good FCS on nearly every PPDU; the frame contents are checked on the first case of every MCS.
#include "halow_sweep.h"
#include <chrono>
using namespace sweep;
#include "../common/test_util.h"

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    int cases = 0;
    for (int w = W1; w <= W4; w++) {
        const double rate = recommendedRateHz(w);
        for (int pre : (w == W1 ? std::vector<int>{Pre1M} : std::vector<int>{PreShort, PreLong})) {
            int worst = 100;
            for (int mcs = 0; mcs <= (w == W1 ? 10 : 9); mcs++) {
                if (!mcsValid(w, mcs, 1)) continue;
                for (int sgi = 0; sgi < 2; sgi++) for (size_t si = 0; si < sets().size(); si++) {
                    HalowGenFrame f;
                    f.psdu = payloadFrame(w == W1 && mcs == 10 ? 30 : 60); f.width = w; f.mcs = mcs; f.pre = pre; f.sgi = sgi;
                    const Result r = run(f, sets()[si], rate, 10);
                    const double need = si == 0 ? 0.9 : 0.7;
                    cases++;
                    CHECK(r.ok >= need * r.sent && r.bad == 0, "%d MHz %s MCS %d sgi %d [%s]: %llu good, %llu bad FCS of %.0f", 1 << w, preambleName(pre), mcs, sgi, sets()[si].name, (unsigned long long)r.ok, (unsigned long long)r.bad, r.sent);
                    worst = std::min(worst, (int)(100 * r.ok / std::max(1.0, r.sent)));
                    if (r.ok) {
                        bool seen = false;
                        for (const auto& fr : r.t.log) if (fr.decoded && fr.widthMhz == 1 << w && fr.mcs == mcs && fr.pre == pre && fr.sgi == (sgi != 0)) { seen = true; break; }
                        CHECK(seen, "%d MHz MCS %d pre %d: the frame log lost width / MCS / preamble / GI", 1 << w, mcs, pre);
                    }
                }
            }
            printf("  %d MHz %s preamble: all valid MCS passed, worst case %d%% of the PPDUs\n", 1 << w, preambleName(pre), worst);
        }
    }
    printf("  %d cases in %.1f s\n", cases, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return testSummary("halow sweep narrow");
}
