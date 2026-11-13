// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/engine/overload_hint.h"
#include <cstdio>
using namespace dect2;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    OverloadHint h(3, 5.0);
    CHECK(!h.update(0, 0)); CHECK(!h.update(2, 1)); CHECK(!h.update(3, 2));   // 3 is not more than 3
    CHECK(h.update(4, 3));                                                    // 4 in window: fires
    CHECK(!h.update(20, 4));                                                  // only once per start
    h.reset();
    CHECK(!h.update(1, 100)); CHECK(!h.update(2, 110)); CHECK(!h.update(3, 120)); CHECK(!h.update(4, 130));   // spread out: never
    h.reset();
    CHECK(h.update(10, 200));                                                 // burst
    printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
