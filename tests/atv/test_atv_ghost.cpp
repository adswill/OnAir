// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analogue TV ghost report: the sync pulse pair in an averaged line, and through the generator and receiver. Simulated.
#include "dect2/atv/atv_ghost.h"
#include "dect2/atv/atv_testkit.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::atvkit;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<float> line(double tau, double a) {
    const double pts = 8; std::vector<float> v(512, 0.f);
    auto pulse = [&](double t0, double amp) { for (int k = 0; k < 512; k++) { const double t = k / pts - 2.0; if (t >= t0 && t < t0 + 4.7) v[(size_t)k] -= (float)(0.4 * amp); } };
    pulse(0, 1); if (a != 0) pulse(tau, a);
    for (int k = (int)(12.5 * pts); k < 480; k++) v[(size_t)k] += 0.5f;   // the average picture: one edge
    return v;
}

int main() {
    for (double tau : {1.5, 2.0, 7.0, 15.0, 30.0}) for (double db : {-6.0, -12.0, -20.0}) {
        const auto g = atvFindGhost(line(tau, std::pow(10.0, db / 20)), 2.0, 8, 4.7);
        CHECK(g.found && std::fabs(g.delayUs - tau) < 0.4 && std::fabs(g.levelDb - db) < 1.5, "tau %.1f db %.0f: found %d %.2f %.1f", tau, db, g.found, g.delayUs, g.levelDb);
    }
    CHECK(!atvFindGhost(line(0, 0), 2.0, 8, 4.7).found, "no ghost, a picture edge is not one");
    for (double d : {1.5, 3.0, 9.0}) {
        // the generator adds the echo at complex baseband, where the vision carrier sits at atvVisionOffsetHz (-2.75 MHz for G): the delay alone
        // turns the echo's carrier phase by 360 f tau (90 deg at 3 us, 270 at 9 us, a quadrature ghost the video cannot show). Cancel that turn so
        // the ghost is in phase with the vision carrier at every delay, which is what the -12 dB check measures.
        AtvFormat gp; atvMakeFormat(kAtvG, kAtvPal, gp);
        AtvGenConfig c; c.echoDb = 12; c.echoDelayUs = d; c.echoPhaseDeg = 360.0 * atvVisionOffsetHz(gp) * d * 1e-6; c.cnrDb = 40;
        const Run r = run(c, 2.0);
        printf("ghost %.1f us -12 dB: found %d, %.2f us, %.1f dB\n", d, r.tel.ghostFound, r.tel.ghostDelayUs, r.tel.ghostDb);
        CHECK(r.tel.ghostFound && std::fabs(r.tel.ghostDelayUs - d) < 0.6 && std::fabs(r.tel.ghostDb + 12) < 4, "through the receiver %.1f", d);
    }
    { AtvGenConfig c; c.cnrDb = 40; const Run r = run(c, 2.0); printf("no ghost: found %d %.2f us %.1f dB\n", r.tel.ghostFound, r.tel.ghostDelayUs, r.tel.ghostDb); CHECK(!r.tel.ghostFound || r.tel.ghostDb < -24, "clean signal"); }
    printf(fails ? "atv_ghost: %d FAILED\n" : "atv_ghost: all passed\n", fails);
    return fails ? 1 : 0;
}
