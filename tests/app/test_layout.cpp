// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Per-mode window layout file names: stable, distinct per mode and sub-mode, safe characters only.
#include "state/layout.h"
#include <cstdio>
int main() {
    int fails = 0;
    auto chk = [&](bool ok, const char* m) { if (!ok) { std::printf("FAIL: %s\n", m); fails++; } };
    chk(layoutFileName(3, "") == "3.ini", "no sub id");
    chk(layoutFileName(3, "dvbt2") == "3-dvbt2.ini", "sub id");
    chk(layoutFileName(3, "a") != layoutFileName(3, "b"), "sub modes distinct");
    chk(layoutFileName(3, "x") != layoutFileName(4, "x"), "families distinct");
    chk(layoutFileName(1, "../etc/x y") == "1-___etc_x_y.ini", "path characters replaced");
    chk(layoutFileName(1, "a") == layoutFileName(1, "a"), "stable");
    std::printf(fails ? "layout: %d failed\n" : "layout: ok\n", fails);
    return fails ? 1 : 0;
}
