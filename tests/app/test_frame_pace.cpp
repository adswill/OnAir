// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The frame loop's redraw policy (app/ui/frame_pace.h): the rates of 0.2.9 stay where nothing new applies, a covered window and the
// battery only ever slow it down, the picture of a playing TV service keeps 60 fps on battery, --shot stays unpaced.
#include "../../app/ui/frame_pace.h"
#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)
static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main() {
    pace::State s;
    CHECK(near(pace::interval(s), 0.1), "idle, focused: 10 fps");
    s.focused = false;
    CHECK(near(pace::interval(s), 0.25), "idle, background: 4 fps");
    s.running = true;
    CHECK(near(pace::interval(s), 1.0 / 15), "receiving, background: 15 fps");
    s.liveWaterfall = true;
    CHECK(near(pace::interval(s), 1.0 / 30), "receiving, background, live waterfall: 30 fps (#246)");
    s.onBattery = true;
    CHECK(near(pace::interval(s), 0.1), "receiving, background, live waterfall, battery saver: 10 fps");
    s.onBattery = false; s.liveWaterfall = false;
    s.focused = true;
    CHECK(near(pace::interval(s), 1.0 / 30), "receiving, focused: 30 fps");
    s.recentInput = true;
    CHECK(near(pace::interval(s), 1.0 / 60), "input: 60 fps");
    s.onBattery = true;
    CHECK(near(pace::interval(s), 1.0 / 30), "input on battery: 30 fps");
    s.batterySaver = false;
    CHECK(near(pace::interval(s), 1.0 / 60), "the battery saver switched off: 60 fps");
    s.batterySaver = true; s.recentInput = false; s.video = true;
    CHECK(near(pace::interval(s), 1.0 / 60), "video on battery keeps 60 fps");
    s.occluded = true;
    CHECK(near(pace::interval(s), 0.1), "a covered window: 10 fps");
    s.popOut = true;
    CHECK(near(pace::interval(s), 1.0 / 60), "the pop-out video keeps playing");
    s = pace::State(); s.shown = false;
    CHECK(near(pace::interval(s), 0.1), "hidden with the CPU saver");
    s.reduceCpuHidden = false;
    CHECK(near(pace::interval(s), 0.25), "hidden without it");
    s.shot = true;
    CHECK(pace::interval(s) == 0, "--shot: unpaced");
    // never faster than before: every combination
    for (int m = 0; m < (1 << 11); m++) {
        pace::State a;
        a.shown = m & 1; a.occluded = m & 2; a.popOut = m & 4; a.noOsWindows = m & 8; a.reduceCpuHidden = m & 16; a.focused = m & 32;
        a.recentInput = m & 64; a.video = m & 128; a.running = m & 256; a.onBattery = m & 512; a.batterySaver = m & 1024;
        pace::State old = a; old.occluded = false; old.onBattery = false;
        CHECK(pace::interval(a) >= pace::interval(old) - 1e-12, "combination %d redraws faster than without occlusion/battery", m);
    }
    {   // the fast waterfall (CW preset): a focused window with a live waterfall draws 60 a second, nothing else changes
        pace::State f;
        f.running = true; f.liveWaterfall = true; f.focused = true;
        CHECK(near(pace::interval(f), 1.0 / 30), "live waterfall, focused: 30 fps");
        f.fastWaterfall = true;
        CHECK(near(pace::interval(f), 1.0 / 60), "fast waterfall, focused: 60 fps");
        f.focused = false;
        CHECK(near(pace::interval(f), 1.0 / 30), "fast waterfall, background: 30 fps as before");
        f.focused = true; f.onBattery = true;
        CHECK(near(pace::interval(f), 1.0 / 30), "fast waterfall on battery with the saver: 30 fps");
        f.onBattery = false; f.liveWaterfall = false;
        CHECK(near(pace::interval(f), 1.0 / 30), "paused: the usual 30 fps");
    }
    std::printf(failures ? "frame pace: %d failure(s)\n" : "frame pace: ok\n", failures);
    return failures ? 1 : 0;
}
