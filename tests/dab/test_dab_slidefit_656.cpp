// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// #656: a DAB slide show picture scales to fit its box (the page or the pop-out window) and keeps its shape.
#include "dect2/dab/slide_fit.h"
#include <cmath>
#include <cstdio>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    CHECK(std::fabs(dabSlideScale(320, 240, 640, 480, 8.f) - 2.f) < 1e-6, "320x240 in 640x480 is 2x");
    CHECK(std::fabs(dabSlideScale(320, 240, 1600, 600, 8.f) - 2.5f) < 1e-6, "the short side decides (2.5x)");
    CHECK(std::fabs(dabSlideScale(320, 240, 4000, 4000, 8.f) - 8.f) < 1e-6, "capped at the maximum");
    CHECK(std::fabs(dabSlideScale(800, 600, 400, 300, 8.f) - 0.5f) < 1e-6, "a large picture shrinks to fit");
    CHECK(dabSlideScale(0, 240, 640, 480, 8.f) == 0.f, "no picture: 0");
    CHECK(dabSlideScale(320, 240, 0, 480, 8.f) == 0.f, "no box: 0");
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
