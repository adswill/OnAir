// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The place of a panel in its own OS window (app/ui/window_place.h, issue #664): saved text, sizes, and a monitor unplugged since.
#include "../../app/ui/window_place.h"
#include <cstdio>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)
using namespace winplace;

int main() {
    const std::vector<Rect> two = {{0, 0, 1920, 1080}, {1920, 0, 1920, 1080}};
    const std::vector<Rect> one = {{0, 0, 1920, 1080}};

    Place p; p.out = true; p.placed = true; p.r = {2100, 100, 800, 600};
    const Place q = parse(write(p));
    CHECK(q.out && q.placed && q.r.x == 2100 && q.r.y == 100 && q.r.w == 800 && q.r.h == 600, "write/parse round trip");
    CHECK(!parse("").out && !parse("garbage").out && !parse("1 1 2 3 4").out && !parse("1 1 2 3 4 5 6").out, "bad text: docked");
    CHECK(parse("1 0 -5 -7 300 200").r.x == -5, "negative positions (a screen to the left) survive");

    Rect r = restore(p, two, 720, 460);
    CHECK(r.x == 2100 && r.y == 100 && r.w == 800 && r.h == 600, "second screen kept while it is plugged in");
    r = restore(p, one, 720, 460);
    CHECK(r.x == (1920 - 800) / 2 && r.y == (1080 - 600) / 2, "second screen unplugged: centred on the first");
    Place far = p; far.r.y = 5000;
    r = restore(far, two, 720, 460);
    CHECK(r.y == (1080 - 600) / 2, "title bar below every screen: centred");
    Place peek = p; peek.r = {1850, 50, 800, 600};   // 70 px on screen 1, the rest on screen 2: reachable
    r = restore(peek, two, 720, 460);
    CHECK(r.x == 1850, "a window across two screens stays");

    Place none;   // never placed (Wayland or first time): default size, middle of the first monitor
    r = restore(none, two, 720, 460);
    CHECK(r.w == 720 && r.h == 460 && r.x == (1920 - 720) / 2 && r.y == (1080 - 460) / 2, "first time: default size, centred");
    CHECK(restore(none, {}, 720, 460).w == 720, "no monitor list: size only");

    Place huge; huge.placed = true; huge.r = {0, 0, 99999, 10};
    r = restore(huge, one, 720, 460);
    CHECK(r.w <= 1920 && r.h >= kMinH, "sizes clamped (limits, then the monitor)");

    if (!failures) std::printf("ok\n");
    return failures ? 1 : 0;
}
