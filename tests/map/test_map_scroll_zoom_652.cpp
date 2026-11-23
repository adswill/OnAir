// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Issue #652: scroll-to-zoom on the maps was far too sensitive with an Apple trackpad or Magic Mouse (smooth deltas, momentum): one level per event. A swipe
// of any size now gives a few levels at most, in proportion to its length; a wheel keeps one level per notch. In memory.
#include "dect2/map/scroll_zoom.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// a swipe: `events` deltas of size `delta` every `dtSec`, then a momentum tail that decays by `decay` per event until it is below 0.001; returns the levels
static int swipe(ScrollZoom& z, double t0, int events, double delta, double dtSec, double decay, double* tEnd = nullptr, double* total = nullptr) {
    int levels = 0;
    double t = t0, d = delta, sum = 0;
    for (int i = 0; i < events; i++, t += dtSec) { levels += z.feed(delta, t); sum += std::fabs(delta); }
    for (d = delta * decay; std::fabs(d) > 0.001; d *= decay, t += dtSec) { levels += z.feed(d, t); sum += std::fabs(d); }
    if (tEnd) *tEnd = t;
    if (total) *total = sum;
    return levels;
}

int main() {
    // a wheel: one notch, one level, as before; a fast spin keeps up
    {
        ScrollZoom z;
        CHECK(z.feed(1.0, 0) == 1 && z.feed(-1.0, 1) == -1, "one notch is one level");
        int n = 0;
        for (int i = 0; i < 6; i++) n += z.feed(1.0, 2 + i * 0.02);
        CHECK(n == 6, "six quick notches are six levels, got %d", n);
        CHECK(z.feed(2.5, 5) == 1, "one big event is one level");
    }
    // trackpad and Magic Mouse swipes of every size: a light touch is 0 or 1 level, never a jump to the limits
    const double kDeltas[] = {0.01, 0.03, 0.1, 0.3};          // per event (GLFW scales precise deltas by 0.1: 0.1 is a fast finger)
    const int kEvents[] = {2, 5, 12, 30, 60};                   // the swipe's length in events at 60 Hz
    const double kDecays[] = {0.0, 0.8, 0.9, 0.95};             // the momentum tail
    for (double dl : kDeltas)
        for (int ev : kEvents)
            for (double dc : kDecays) {
                ScrollZoom z;
                double t1 = 0, total = 0;
                const int up = swipe(z, 0, ev, dl, 1.0 / 60, dc, &t1, &total);
                ScrollZoom z2;
                const int down = swipe(z2, 0, ev, -dl, 1.0 / 60, dc);
                const int cap = 1 + (int)std::ceil(t1 / ScrollZoom::kMinGapSec);   // never faster than one level per kMinGapSec
                CHECK(up >= 0 && down <= 0 && up == -down, "delta %.2f x %d tail %.2f: up %d, down %d (symmetric)", dl, ev, dc, up, down);
                CHECK(up <= cap, "delta %.2f x %d tail %.2f: %d levels in %.2f s (cap %d)", dl, ev, dc, up, t1, cap);
                if (total < 0.5) CHECK(up == 0, "delta %.2f x %d tail %.2f: a touch of %.2f notches zooms %d levels", dl, ev, dc, total, up);
                if (total < 2.5) CHECK(up <= 2, "delta %.2f x %d tail %.2f: %.2f notches zoom %d levels", dl, ev, dc, total, up);
            }
    // a longer swipe zooms more than a light one (proportional, not all or nothing)
    {
        ScrollZoom a, b;
        const int light = swipe(a, 0, 10, 0.05, 1.0 / 60, 0.0), hard = swipe(b, 0, 60, 0.1, 1.0 / 60, 0.9);
        CHECK(hard > light && light <= 1, "light swipe %d levels, hard swipe %d", light, hard);
    }
    // two swipes with a pause: each counts alone, and a leftover does not carry over
    {
        ScrollZoom z;
        double t = 0;
        const int a = swipe(z, t, 8, 0.1, 1.0 / 60, 0.0, &t);
        t += 1.0;
        const int b = swipe(z, t, 8, 0.1, 1.0 / 60, 0.0);
        CHECK(a == b && a <= 1, "the same swipe twice: %d then %d levels", a, b);
    }
    // turning round drops the leftover; garbage events do nothing
    {
        ScrollZoom z;
        CHECK(z.feed(0.6, 0) == 0, "0.6 waits");
        CHECK(z.feed(-0.6, 0.01) == 0, "turning round starts again");
        CHECK(z.feed(0.0, 0.02) == 0 && z.feed(NAN, 0.03) == 0, "zero and NaN do nothing");
    }
    // a Magic Mouse fling of 300 events at the largest smooth delta cannot run to the limits (17 levels from 2 to 19)
    {
        ScrollZoom z;
        double t1 = 0;
        const int levels = swipe(z, 0, 300, 0.3, 1.0 / 120, 0.97, &t1);
        CHECK(levels <= 1 + (int)std::ceil(t1 / ScrollZoom::kMinGapSec), "an enormous fling gives %d levels over its %.1f s", levels, t1);
        CHECK(levels >= 1, "but it still zooms");
    }
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
