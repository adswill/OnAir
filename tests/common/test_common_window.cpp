// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// dect2::window::besselI0 against the original private copies (each with its own term count / early-exit tolerance),
// compared bit-exactly on random arguments from 0 to 80 plus edge values.
#include "dect2/common/window.h"
#include "dect2/common/stable_random.h"
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static bool same(double a, double b) { return !memcmp(&a, &b, sizeof a); }

int main() {
    std::mt19937_64 r(3);
    dect2::uniform_real_distribution<double> u(0, 80);
    for (int it = 0; it < 20000; it++) {
        double x = it < 6 ? (it == 0 ? 0.0 : it == 1 ? 1e-9 : it == 2 ? 1.0 : it == 3 ? 100.0 : it == 4 ? -3.5 : 700.0) : u(r);
        { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } CHECK(same(s, window::besselI0(x, 40, 0.0)), "40 plain x=%g", x); }
        { double s = 1, t = 1; for (int k = 1; k < 60; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; if (t < 1e-14 * s) break; } CHECK(same(s, window::besselI0(x, 60, 1e-14)), "60 1e-14"); }
        { double s = 1, t = 1; for (int k = 1; k < 50; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } CHECK(same(s, window::besselI0(x, 50, 0.0)), "50"); }
        { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } CHECK(same(s, window::besselI0(x, 40, 0.0)), "40 int2k"); }
        { double s = 1, t = 1; for (int k = 1; k < 30; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } CHECK(same(s, window::besselI0(x, 30, 0.0)), "30"); }
        { double s = 1, t = 1; for (int k = 1; k < 60; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; } CHECK(same(s, window::besselI0(x, 60, 1e-12)), "60 1e-12"); }
        { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; } CHECK(same(s, window::besselI0(x, 40, 1e-12)), "40 1e-12"); }
        { double sum = 1, term = 1; for (int k = 1; k < 64; k++) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; if (term < sum * 1e-17) break; } CHECK(same(sum, window::besselI0(x, 64, 1e-17)), "64 1e-17"); }
    }
    printf(fails ? "test_common_window: %d FAILED\n" : "test_common_window: OK\n", fails);
    return fails ? 1 : 0;
}
