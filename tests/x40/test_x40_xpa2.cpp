// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// XPA2 (16-tone MFSK, 7.8125 Bd). What is checked:
//  1. messages built from the format as Rivet reads it (start tones, sync, five-figure groups with the repeat tone, the end) through the
//     receiver with impairments: dial error, off-centre tuning (+700 Hz), sender clock error, drift, lower sideband (Rivet: "inverted"), 48 ksps;
//  2. a noise sweep (dial error and clock error): share of the groups read right, the 50 % and 90 % points;
//  3. noise alone and a carrier must give no groups (false decodes counted).
// Simulated signals only: no recording of XPA2 was available.
#include "x40_gen_rivet.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static Path path(double off, double drift = 0, double ppm = 0, bool lower = false) { Path p; p.offsetHz = off; p.driftHzPerSec = drift; p.ppm = ppm; p.lower = lower; return p; }
static int decodeKinds(const X40Telemetry& t) { return countKind(t, "Groups") + countKind(t, "Start") + countKind(t, "End"); }

int main() {
    const auto g = randomGroups(21, 24);
    struct Case { const char* what; double fs; Path p; };
    const Case cases[] = {
        {"8 ksps", 8000, path(0)},
        {"dial +4.7 Hz", 8000, path(4.7)},
        {"off-centre +700 Hz, 48 ksps", 48000, path(700)},
        {"clock +300 ppm, drift -0.05 Hz/s", 8000, path(-33, -0.05, 300)},
        {"lower sideband (inverted), dial +3 Hz", 8000, path(3, 0, 0, true)},
    };
    for (const Case& c : cases) {
        auto x = xpaSignal(g, true, 7.8125, c.fs, c.p);
        addSnr(x, 0, c.fs, 3);
        const auto t = run(x40::Xpa2, x, c.fs);
        const int f = groupsFound(g, rowText(t, "Groups"));
        printf("  %-40s %2d / %zu groups, start %d, end %d | %s\n", c.what, f, g.size(), countKind(t, "Start"), countKind(t, "End"), t.headline.c_str());
        CHECK(f >= (int)g.size() - 1, "%s: %d of %zu groups", c.what, f, g.size());
        CHECK(countKind(t, "End") >= 1, "%s: no end of message", c.what);
    }
    printf("noise sweep (dial +2.1 Hz, clock +100 ppm): groups right\n");
    std::vector<std::pair<double, double>> pts;
    for (double snr = -26; snr <= -13.9; snr += 2) {
        int ok = 0, n = 0;
        for (unsigned seed : {1u, 2u}) {
            const auto gs = randomGroups(200 + seed, 24);
            auto x = xpaSignal(gs, true, 7.8125, 8000, path(2.1, 0, 100));
            addSnr(x, snr, 8000, 60 + seed);
            const auto t = run(x40::Xpa2, x, 8000);
            ok += groupsFound(gs, rowText(t, "Groups")); n += (int)gs.size();
        }
        pts.push_back({snr, (double)ok / n});
        printf("  %6.1f dB: %3d / %d\n", snr, ok, n);
    }
    printf("  XPA2: 50 %% of the groups at %.1f dB, 90 %% at %.1f dB (signal to noise in 3 kHz)\n", snrAt(pts, 0.5), snrAt(pts, 0.9));
    CHECK(pts.back().second >= 0.9, "%.2f of the groups at %.0f dB", pts.back().second, pts.back().first);
    int falseRows = 0;
    for (unsigned seed : {7u, 8u}) { std::vector<cf32> n((size_t)(8000 * 60), cf32(0, 0)); addNoise(n, 1.0, seed); const auto t = run(x40::Xpa2, n, 8000); falseRows += decodeKinds(t); }
    { std::vector<std::pair<double, double>> seg = {{1100, 40.0}}; auto x = toneSeq(seg, 8000, path(0)); addSnr(x, 0, 8000, 9); const auto t = run(x40::Xpa2, x, 8000); falseRows += decodeKinds(t); }
    printf("false decodes (2 x 60 s noise, a carrier): %d rows\n", falseRows);
    CHECK(falseRows == 0, "%d rows from noise or a carrier", falseRows);
    printf(fails ? "x40 xpa2: %d FAILED\n" : "x40 xpa2: ok\n", fails);
    return fails ? 1 : 0;
}
