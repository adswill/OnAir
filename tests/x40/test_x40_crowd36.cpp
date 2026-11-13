// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CROWD36 (34-tone MFSK, 40 Bd). What is checked:
//  1. messages built from the format as Rivet reads it (the two sync tones with tone 24 high, the letter shift, five-letter groups, line ends)
//     through the receiver with impairments: dial error, off-centre tuning (-800 Hz), sender clock error, drift, 48 and 250 ksps;
//  2. a noise sweep (dial error and clock error): share of the groups read right, the 50 % and 90 % points;
//  3. noise alone and a carrier must give no text (false decodes counted).
// The low sync tone of real stations is not known here (Rivet needs only the high one); the tests use tone 10. Simulated signals only.
#include "x40_gen_rivet.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static Path path(double off, double drift = 0, double ppm = 0) { Path p; p.offsetHz = off; p.driftHzPerSec = drift; p.ppm = ppm; return p; }

int main() {
    const auto g = randomGroups(31, 40, true);
    struct Case { const char* what; double fs, rxOff; Path p; };
    const Case cases[] = {
        {"8 ksps", 8000, 0, path(0)},
        {"dial +13.6 Hz", 8000, 0, path(13.6)},
        {"off-centre -800 Hz, 48 ksps", 48000, 0, path(-800)},
        {"clock -500 ppm, drift +0.2 Hz/s", 8000, 0, path(17, 0.2, -500)},
        {"250 ksps, receiver offset +45 kHz", 250000, 45000, path(45000 - 9)},
    };
    for (const Case& c : cases) {
        auto x = crowd36Signal(g, c.fs, c.p);
        addSnr(x, 10, c.fs, 3);
        const auto t = run(x40::Crowd36, x, c.fs, c.rxOff);
        const int f = groupsFound(g, rowText(t, "Text"));
        printf("  %-40s %2d / %zu groups, sync %d | %s\n", c.what, f, g.size(), countKind(t, "Sync"), t.headline.c_str());
        if (f < (int)g.size() - 1) printf("%s", rowText(t, "Text").c_str());
        CHECK(f >= (int)g.size() - 1, "%s: %d of %zu groups", c.what, f, g.size());
        CHECK(countKind(t, "Sync") >= 1, "%s: no sync", c.what);
    }
    printf("noise sweep (dial -7 Hz, clock +100 ppm): groups right\n");
    std::vector<std::pair<double, double>> pts;
    for (double snr = -16; snr <= -1.9; snr += 2) {
        int ok = 0, n = 0;
        for (unsigned seed : {1u, 2u}) {
            const auto gs = randomGroups(300 + seed, 40, true);
            auto x = crowd36Signal(gs, 8000, path(-7, 0, 100));
            addSnr(x, snr, 8000, 70 + seed);
            const auto t = run(x40::Crowd36, x, 8000);
            ok += groupsFound(gs, rowText(t, "Text")); n += (int)gs.size();
        }
        pts.push_back({snr, (double)ok / n});
        printf("  %6.1f dB: %3d / %d\n", snr, ok, n);
    }
    printf("  CROWD36: 50 %% of the groups at %.1f dB, 90 %% at %.1f dB (signal to noise in 3 kHz)\n", snrAt(pts, 0.5), snrAt(pts, 0.9));
    CHECK(pts.back().second >= 0.9, "%.2f of the groups at %.0f dB", pts.back().second, pts.back().first);
    int falseRows = 0;
    for (unsigned seed : {7u, 8u}) { std::vector<cf32> n((size_t)(8000 * 60), cf32(0, 0)); addNoise(n, 1.0, seed); const auto t = run(x40::Crowd36, n, 8000); falseRows += countKind(t, "Text") + countKind(t, "Sync"); }
    { std::vector<std::pair<double, double>> seg = {{2000, 40.0}}; auto x = toneSeq(seg, 8000, path(0)); addSnr(x, 0, 8000, 9); const auto t = run(x40::Crowd36, x, 8000); falseRows += countKind(t, "Text") + countKind(t, "Sync"); }
    printf("false decodes (2 x 60 s noise, a carrier): %d rows\n", falseRows);
    CHECK(falseRows == 0, "%d rows from noise or a carrier", falseRows);
    printf(fails ? "x40 crowd36: %d FAILED\n" : "x40 crowd36: ok\n", fails);
    return fails ? 1 : 0;
}
