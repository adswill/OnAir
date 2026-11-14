// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Workspace window fields: encode/decode round trip, records without them, and the clamp into the monitors' work areas.
#include "state/workspace_record.h"
#include <cstdio>
using namespace wsrec;
int main() {
    int fails = 0;
    auto chk = [&](bool ok, const char* m) { if (!ok) { std::printf("FAIL: %s\n", m); fails++; } };
    WindowState s; s.have = true; s.w = 1280; s.h = 720; s.x = -1500; s.y = 40; s.hasPos = true; s.maximized = false;
    const std::string rec = escapeField("a|b") + "|" + encodeWindow(s);
    const auto f = splitRecord(rec);
    chk(f.size() == 7 && f[0] == "a|b", "escape kept, window fields split");
    const WindowState d = decodeWindow(f, 1);
    chk(d.have && d.w == 1280 && d.h == 720 && d.x == -1500 && d.y == 40 && d.hasPos && !d.maximized, "round trip");
    s.maximized = true; s.hasPos = false;
    const WindowState m = decodeWindow(splitRecord(encodeWindow(s)), 0);
    chk(m.have && m.maximized && !m.hasPos, "maximized, no position");
    chk(!decodeWindow(splitRecord(""), 0).have, "empty string = no window");
    chk(!decodeWindow(splitRecord("x|y|z"), 0).have, "old record without the fields");
    chk(!decodeWindow(splitRecord("10|10|0|0|1|0"), 0).have, "tiny size refused");
    chk(encodeWindow(WindowState()).empty(), "not captured encodes empty");
    const std::vector<Rect> one{{0, 0, 1920, 1080}};
    Rect c = clampToWorkAreas({100, 100, 800, 600}, one);
    chk(c.x == 100 && c.y == 100 && c.w == 800 && c.h == 600, "inside: unchanged");
    c = clampToWorkAreas({1800, 1000, 800, 600}, one);
    chk(c.x == 1120 && c.y == 480 && c.w == 800 && c.h == 600, "hanging off: moved in");
    c = clampToWorkAreas({-50, -20, 3000, 2000}, one);
    chk(c.x == 0 && c.y == 0 && c.w == 1920 && c.h == 1080, "too big: cut to the area");
    const std::vector<Rect> two{{0, 0, 1920, 1080}, {1920, 0, 1280, 1024}};
    c = clampToWorkAreas({2000, 100, 800, 600}, two);
    chk(c.x == 2000 && c.w == 800, "second monitor kept");
    c = clampToWorkAreas({-3000, 100, 800, 600}, two);
    chk(c.x == 0 && c.y == 100, "monitor gone: pulled into the first");
    chk(clampToWorkAreas({5, 6, 7, 8}, {}).x == 5, "no areas: as is");
    std::printf(fails ? "workspace_window: %d failed\n" : "workspace_window: ok\n", fails);
    return fails ? 1 : 0;
}
