// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The transponder detector of whole-span DVB-S/S2 (core/src/engine/transponders.cpp) on synthetic spectra: several carriers, weak and strong, adjacent,
// a tilted floor, the SDR's DC spike, and things that are not transponders.
#include "dect2/engine/transponders.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Car { double fc, rs, snrDb, alpha; };
static const int N = 4096;
static const double RATE = 40e6;

// raised cosine power spectrum of a root raised cosine carrier: flat to (1-a)Rs/2, cosine shoulder to (1+a)Rs/2
static double rcShape(double f, double rs, double a) {
    const double x = std::fabs(f), f1 = (1 - a) * rs / 2, f2 = (1 + a) * rs / 2;
    if (x <= f1) return 1;
    if (x >= f2) return 0;
    return 0.5 * (1 + std::cos(M_PI * (x - f1) / (f2 - f1)));
}

static std::vector<float> makePsd(const std::vector<Car>& cars, double floorDb = -60, double tiltDb = 0, double dcSpikeDb = 0, unsigned seed = 1) {
    std::vector<float> v((size_t)N);
    srand(seed);
    for (int k = 0; k < N; k++) {
        const double f = (k - N / 2) * RATE / N;
        double p = std::pow(10.0, (floorDb + tiltDb * (k - N / 2) / (double)N) / 10);
        double s = 0;
        for (const Car& c : cars) s += std::pow(10.0, (floorDb + c.snrDb) / 10) * rcShape(f - c.fc, c.rs, c.alpha);
        const double ripple = ((rand() / (double)RAND_MAX) - 0.5) * 1.0;   // +-0.5 dB of what an average of a few dozen frames still shows
        v[(size_t)k] = (float)(10 * std::log10(p + s) + ripple);
    }
    if (dcSpikeDb > 0) for (int k = N / 2 - 1; k <= N / 2 + 1; k++) v[(size_t)k] = (float)(floorDb + dcSpikeDb);
    return v;
}

static const Transponder* near(const std::vector<Transponder>& t, double fc, double tol = 400e3) {
    for (const Transponder& x : t) if (std::fabs(x.centerHz - fc) < tol) return &x;
    return nullptr;
}

int main() {
    TransponderParams p;
    {   // three carriers of different rates, one far off centre
        auto t = detectTransponders(makePsd({{-11e6, 4.99e6, 18, 0.35}, {2.5e6, 2.0e6, 14, 0.25}, {13e6, 8e6, 20, 0.2}}), RATE, p);
        CHECK(t.size() == 3, "three carriers: found %zu", t.size());
        const double fc[3] = {-11e6, 2.5e6, 13e6}, rs[3] = {4.99e6, 2.0e6, 8e6};
        for (int i = 0; i < 3; i++) {
            const Transponder* x = near(t, fc[i]);
            CHECK(x != nullptr, "carrier %d missing", i);
            if (!x) continue;
            CHECK(std::fabs(x->centerHz - fc[i]) < 120e3, "carrier %d centre %.0f", i, x->centerHz);
            CHECK(std::fabs(x->rsHz / rs[i] - 1) < 0.06, "carrier %d symbol rate %.0f vs %.0f", i, x->rsHz, rs[i]);
        }
    }
    {   // a weak carrier (7 dB over the floor) beside a strong one, and a tilted floor
        auto t = detectTransponders(makePsd({{-9e6, 4e6, 7, 0.35}, {6e6, 5e6, 25, 0.35}}, -60, 6.0), RATE, p);
        CHECK(near(t, -9e6) != nullptr, "weak carrier lost on a tilted floor");
        CHECK(near(t, 6e6) != nullptr, "strong carrier lost");
        CHECK(t.size() == 2, "weak + strong: %zu", t.size());
    }
    {   // adjacent transponders: 0.3 MHz between the edges, and edge to edge with a dip
        auto t = detectTransponders(makePsd({{-4e6, 6e6, 20, 0.2}, {2.45e6, 6e6, 20, 0.2}}), RATE, p);
        CHECK(t.size() == 2, "adjacent with a guard: %zu", t.size());
        if (t.size() == 2) CHECK(std::fabs(t[0].rsHz / 6e6 - 1) < 0.08 && std::fabs(t[1].rsHz / 6e6 - 1) < 0.08, "adjacent widths %.0f %.0f", t[0].rsHz, t[1].rsHz);
        auto u = detectTransponders(makePsd({{-3.5e6, 5.9e6, 20, 0.35}, {3.5e6, 5.9e6, 20, 0.35}}), RATE, p);   // shoulders overlap: a dip of 6 dB at the join, not a gap
        CHECK(u.size() == 2, "abutting: %zu", u.size());
        // mixed levels: a 12 dB step between neighbours
        auto m = detectTransponders(makePsd({{-4e6, 6e6, 28, 0.2}, {3.2e6, 6e6, 16, 0.2}}), RATE, p);
        CHECK(m.size() == 2, "mixed levels: %zu", m.size());
    }
    {   // the DC spike is not a transponder; a carrier on DC survives it
        auto t = detectTransponders(makePsd({}, -60, 0, 25), RATE, p);
        CHECK(t.empty(), "DC spike alone: %zu found", t.size());
        auto u = detectTransponders(makePsd({{0, 5e6, 18, 0.35}}, -60, 0, 30), RATE, p);
        CHECK(u.size() == 1 && std::fabs(u[0].centerHz) < 150e3 && std::fabs(u[0].rsHz / 5e6 - 1) < 0.06, "carrier on DC with its spike: %zu found", u.size());
        auto w = detectTransponders(makePsd({{7e6, 4e6, 18, 0.35}}, -60, 0, 25), RATE, p);
        CHECK(w.size() == 1, "carrier plus DC spike: %zu", w.size());
    }
    {   // two overlapping false peaks (narrow spurs 200 kHz apart) are not merged into a transponder; nor is one tone
        std::vector<float> v = makePsd({});
        for (int k : {1500, 1503, 1506, 1550, 1553}) v[(size_t)k] += 0;   // keep the floor
        for (int k = 1495; k <= 1509; k++) v[(size_t)k] = -60 + 22;
        for (int k = 1530; k <= 1544; k++) v[(size_t)k] = -60 + 19;
        auto t = detectTransponders(v, RATE, p);
        CHECK(t.empty(), "two close spurs became %zu transponders", t.size());
        std::vector<float> one = makePsd({});
        for (int k = 3000; k <= 3004; k++) one[(size_t)k] = -60 + 30;
        CHECK(detectTransponders(one, RATE, p).empty(), "a tone became a transponder");
    }
    CHECK(detectTransponders(makePsd({}), RATE, p).empty(), "noise only");
    CHECK(detectTransponders({}, RATE, p).empty(), "empty input");
    {   // a 2 Msym/s carrier is found with a lower limit and ignored with a higher one
        TransponderParams q; q.minRsHz = 3e6;
        CHECK(detectTransponders(makePsd({{0, 2e6, 20, 0.35}}), RATE, q).empty(), "too narrow for the limit");
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("transponders: all passed\n");
    return 0;
}
