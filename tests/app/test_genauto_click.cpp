// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// #595: a click that began on a list or panel over the spectrum (Tuning step, Auto notch) must not press a General / Auto label under it.
#include "../../app/specx/general_auto_click.h"
#include <cstdio>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    using dect2::genauto::ClickGate;
    {   // an ordinary click on the plot counts
        ClickGate g; g.frame(true, true);
        CHECK(g.accept(true, true), "press and release on the plot is a click");
    }
    {   // the press was on a combo list over the plot (the plot is not hovered), the release lands on the label under it
        ClickGate g; g.frame(true, false);
        CHECK(!g.accept(true, true), "a press on a list over the plot must not click the label under it");
    }
    {   // the release is blocked by a popup
        ClickGate g; g.frame(true, true);
        CHECK(!g.accept(true, false), "a release under a popup is no click");
    }
    {   // a stale press does not carry to the next release
        ClickGate g; g.frame(true, true); (void)g.accept(true, true);
        CHECK(!g.accept(true, true), "a release without a new press is no click");
    }
    {   // no release: nothing
        ClickGate g; g.frame(true, true);
        CHECK(!g.accept(false, true), "a held button is no click yet");
    }
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("genauto click: ok\n");
    return 0;
}
