// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Weather fax hand phasing: shift wraps and round-trips, and the automatic search puts a synthetic black bar at the left edge.
#include "dect2/marine/wefax_phase.h"
#include <cstdint>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<uint8_t> shifted(const std::vector<uint8_t>& p, int w, int h, int s) {
    std::vector<uint8_t> o;
    wefaxPhaseShift(p.data(), w, h, s, o);
    return o;
}

int main() {
    const std::vector<uint8_t> p = {1, 2, 3, 4, 5, 6};   // 3 x 2
    CHECK(shifted(p, 3, 2, 1) == (std::vector<uint8_t>{3, 1, 2, 6, 4, 5}), "shift 1 right");
    CHECK(shifted(p, 3, 2, 0) == p && shifted(p, 3, 2, 3) == p && shifted(p, 3, 2, -3) == p, "0 and full width");
    CHECK(shifted(p, 3, 2, -1) == (std::vector<uint8_t>{2, 3, 1, 5, 6, 4}), "shift 1 left");
    for (int s = -7; s <= 7; s++) {
        auto a = shifted(p, 3, 2, s);
        CHECK(shifted(a, 3, 2, -s) == p, "round trip");
    }
    CHECK(wefaxPhaseWrap(-1, 10) == 9 && wefaxPhaseWrap(23, 10) == 3 && wefaxPhaseWrap(5, 0) == 0, "wrap");
    CHECK(shifted({}, 0, 0, 5).empty(), "empty picture");

    // 400 x 20 picture, grey 200 with texture, a black bar 6 columns wide starting at column 150
    const int w = 400, h = 20;
    std::vector<uint8_t> pic((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) pic[(size_t)y * w + x] = (uint8_t)(190 + ((x * 7 + y * 3) % 40));
    for (int y = 0; y < h; y++)
        for (int x = 150; x < 156; x++) pic[(size_t)y * w + x] = 0;
    const int s = wefaxPhaseAuto(pic.data(), w, h);
    CHECK(s == w - 150, "auto shift for the bar at 150");
    auto fixed = shifted(pic, w, h, s);
    bool bar = true;
    for (int y = 0; y < h; y++) for (int x = 0; x < 6; x++) if (fixed[(size_t)y * w + x] != 0) bar = false;
    CHECK(bar, "bar now at the left edge");
    CHECK(wefaxPhaseAuto(fixed.data(), w, h) == 0, "already phased gives 0");
    std::vector<uint8_t> flat((size_t)w * h, 128);
    CHECK(wefaxPhaseAuto(flat.data(), w, h) == 0, "no contrast gives 0");
    return testSummary("marine_wefax_phase");
}
