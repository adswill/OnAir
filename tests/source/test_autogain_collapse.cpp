// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Auto must not take the gain down to the radio's minimum (user report 2026-11-15: "automatic gain always reduces the gain to 0 while it should sit close to
// the ADC green limit"). Normal bands (a few mid-strength signals, a weak wanted signal) on every radio model, with the default settings (Auto v2) and with
// the old headroom loop: the gain that Auto settles at must not be the minimum unless the ADC really clips (or has no headroom) there.
#include "autogain_sim.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dect2;
using namespace dect2::ag;
using namespace agsim;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Scene { const char* name; int std_; double bw, rate, off, sig; std::vector<Tone> blockers; };

int main() {
    const std::vector<Scene> scenes = {
        {"DMR weak, 3 mid signals", 11, 12.5e3, 2.4e6, 100e3, -112, {{-600e3, -62}, {350e3, -55}, {820e3, -68}}},
        {"DMR weak, one -45 dBm neighbour", 11, 12.5e3, 2.4e6, 100e3, -108, {{500e3, -45}}},
        {"DMR weak, quiet band", 11, 12.5e3, 2.4e6, 100e3, -112, {}},
        {"DMR medium, 2 mid signals", 11, 12.5e3, 2.4e6, 100e3, -85, {{-700e3, -58}, {450e3, -60}}},
        {"FM broadcast, 4 stations", 7, 200e3, 2.4e6, 0, -70, {{-800e3, -50}, {-400e3, -45}, {500e3, -52}, {900e3, -58}}},
        {"DVB-T, 3 mid signals", 1, 8e6, 9.14e6, 0, -72, {{-3.5e6, -55}, {3e6, -60}}},
    };
    const char* radios[] = {"hackrf", "rtl-r820t", "rtl-blog-v4", "airspy", "sdrplay", "pluto", "generic", "rtltcp", "airspyhf"};
    for (const char* id : radios) {
        const RadioProfile rp = *radioProfileById(id);
        for (const Scene& sc : scenes) {
            for (int v2 = 0; v2 < 2; v2++) {
                const double mid = (rp.minTotal(false) + rp.maxTotal(false)) / 2;
                SimRadio sim(rp, mid);
                sim.rateHz = sc.rate; sim.chanBw = sc.bw; sim.chanOff = sc.off; sim.sigDbm = sc.sig; sim.blockers = sc.blockers;
                const ModeTarget tg = modeTargetFor(sc.std_, sc.bw, sc.rate);
                AutoGainController c;
                AgSettings s; s.mode = AgMode::Auto; s.v2 = v2 != 0;
                c.configure(rp, tg, s);
                double now = 0;
                c.start(now, sim.live.totalDb, false, &sim.live);
                double minSeen = 1e9; int downs = 0;
                run(c, sim, now, 300, [&](double) { minSeen = std::min(minSeen, c.current().totalDb); });
                const double g = c.current().totalDb, lo = rp.minTotal(false), hi = rp.maxTotal(false);
                // what the ADC reads at the minimum gain: does it clip / lack headroom there?
                SimRadio at(rp, lo); at.rateHz = sc.rate; at.chanBw = sc.bw; at.chanOff = sc.off; at.sigDbm = sc.sig; at.blockers = sc.blockers; at.jitterDb = 0;
                at.adc(); const BlockStats bmin = at.adc();
                const bool minClips = bmin.clipFraction > tg.clipLimit || bmin.headroomDb < tg.minHeadroomDb;
                const double headNow = c.meters().headroomDb;
                printf("%-12s v2=%d %-34s final %6.1f dB (range %.0f..%.0f, lowest seen %.1f) headroom %.1f (floor %.0f, target %.0f) %s\n", id, v2, sc.name, g, lo, hi, minSeen, headNow,
                       tg.minHeadroomDb, tg.headroomDb, c.status().c_str());
                (void)downs;
                if (!minClips) CHECK(g > lo + 0.25 * (hi - lo) || g > lo + 12, "%s v2=%d '%s': settled at %.1f dB, near the minimum %.0f (headroom at the minimum is fine)", id, v2, sc.name, g, lo);
            }
        }
    }
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
