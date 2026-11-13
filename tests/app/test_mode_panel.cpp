// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The pure parts of the per-mode Simple / Advanced view (app/ui/kit/mode_panel_logic.h).
#include "../../app/ui/kit/mode_panel_logic.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
using namespace modepanel;

int main() {
    CHECK(prefKey(35, 0) == "uiLevel.35", "family key %s", prefKey(35, 0).c_str());
    CHECK(prefKey(25, 3) == "uiLevel.25.3", "sub key %s", prefKey(25, 3).c_str());
    CHECK(prefKey(9, 0) != prefKey(9, 1) && prefKey(9, 1) != prefKey(1, 9), "keys are distinct");
    CHECK(parseLevel("simple", Level::Advanced) == Level::Simple, "simple");
    CHECK(parseLevel("advanced", Level::Simple) == Level::Advanced, "advanced");
    CHECK(parseLevel("", Level::Advanced) == Level::Advanced && parseLevel("junk", Level::Simple) == Level::Simple, "unknown gives the default");
    CHECK(parseLevel(levelText(Level::Simple), Level::Advanced) == Level::Simple && parseLevel(levelText(Level::Advanced), Level::Simple) == Level::Advanced, "round trip");
    CHECK(profileDefault(true) == Level::Advanced, "an existing profile keeps the tabs");
    CHECK(profileDefault(false) == Level::Simple, "a new profile starts Simple");

    // layout of the panes
    PaneLayout l = layoutPanes(1000, 10, {1, 1}, 330);
    CHECK(l.perRow == 2 && std::fabs(l.widths[0] + l.widths[1] + 10 - 1000) < 0.01f, "two panes side by side: %d %.1f %.1f", l.perRow, l.widths[0], l.widths[1]);
    l = layoutPanes(1000, 10, {2, 1}, 330);
    CHECK(std::fabs(l.widths[0] - 2 * l.widths[1]) < 0.01f, "weights 2:1: %.1f %.1f", l.widths[0], l.widths[1]);
    l = layoutPanes(500, 10, {1, 1}, 330);
    CHECK(l.perRow == 1 && l.widths[0] == 500 && l.widths[1] == 500, "a narrow area stacks: %d", l.perRow);
    l = layoutPanes(700, 10, {1, 1, 1}, 330);
    CHECK(l.perRow == 2 && l.widths.size() == 3 && std::fabs(l.widths[2] - 700) < 0.01f, "three panes in a medium area: two, then one full row (%d, %.1f)", l.perRow, l.widths[2]);
    l = layoutPanes(1500, 10, {1, 1, 1}, 330);
    CHECK(l.perRow == 3, "three side by side");
    l = layoutPanes(0, 10, {1, 1}, 330);
    CHECK(l.perRow == 1 && l.widths[0] >= 1, "zero width does not break");
    CHECK(layoutPanes(800, 10, {}, 330).widths.empty(), "no panes");
    CHECK(layoutPanes(800, 10, {0, 0}, 330).widths.size() == 2, "zero weights are lifted");
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("mode_panel: ok\n");
    return 0;
}
