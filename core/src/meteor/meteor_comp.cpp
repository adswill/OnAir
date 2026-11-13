// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor-M LRPT colour composites: see meteor_comp.h.
#include "dect2/meteor/meteor_comp.h"
#include <algorithm>

namespace dect2 {

namespace {
bool hasCh(const MeteorImage& im, int c) { return im.packets[(size_t)c] > 0; }
const MeteorStrip* stripOf(const MeteorImage& im, int c, int row) {
    const auto& v = im.ch[(size_t)c];
    return row >= 0 && row < (int)v.size() ? v[(size_t)row].get() : nullptr;
}
} // namespace

std::array<int, 3> meteorViewChannels(const MeteorImage* im, int view) {
    if (view == 9) return {0, 1, 3};
    if (view == kMeteorViewVegetation) return {1, 0, 0};   // near-infrared in red, the visible channel in green and blue: vegetation shows red
    if (view == kMeteorViewAvhrr221) return {1, 1, 0};     // AVHRR 221: channel 2 in red and green, channel 1 in blue
    if (view >= 3 && view <= 8) { const int c = view - 3; return {c, c, c}; }
    if (view == 1) return {0, 1, 2};
    if (view == 2) return {0, 1, 4};
    if (!im) return {0, 1, 2};
    if (hasCh(*im, 0) && hasCh(*im, 1) && hasCh(*im, 2)) return {0, 1, 2};   // day
    if (hasCh(*im, 0) && hasCh(*im, 1) && hasCh(*im, 4)) return {0, 1, 4};   // night: the infrared channel instead of 3
    if (hasCh(*im, 0) && hasCh(*im, 1) && hasCh(*im, 3)) return {0, 1, 3};   // APID 64, 65 and 67 (channel 3 is not sent)
    for (int c = 0; c < 6; c++) if (hasCh(*im, c)) return {c, c, c};
    return {0, 1, 2};
}

uint32_t meteorThermal(uint8_t g) {
    const double t = 1.0 - g / 255.0;   // 0 cold .. 1 warm (inverted grey: the infrared channel shows cold clouds bright)
    static const double st[6][3] = {{255, 255, 255}, {200, 40, 40}, {240, 220, 40}, {60, 200, 80}, {40, 200, 220}, {20, 40, 140}};
    const double x = t * 5; const int i = std::min(4, (int)x); const double f = x - i;
    const uint32_t r = (uint32_t)(st[i][0] + (st[i + 1][0] - st[i][0]) * f), gg = (uint32_t)(st[i][1] + (st[i + 1][1] - st[i][1]) * f), b = (uint32_t)(st[i][2] + (st[i + 1][2] - st[i][2]) * f);
    return 0xFF000000u | b << 16 | gg << 8 | r;
}

void meteorComposeRow(const MeteorImage& im, const std::array<int, 3>& ch, int row, const uint8_t* lut, bool thermalSingle, uint32_t* px) {
    const MeteorStrip* s[3] = {stripOf(im, ch[0], row), stripOf(im, ch[1], row), stripOf(im, ch[2], row)};
    for (int i = 0; i < 8 * kMeteorWidth; i++) {
        const uint32_t r = s[0] ? lut[(size_t)ch[0] * 256 + s[0]->px[(size_t)i]] : 0, g = s[1] ? lut[(size_t)ch[1] * 256 + s[1]->px[(size_t)i]] : 0, b = s[2] ? lut[(size_t)ch[2] * 256 + s[2]->px[(size_t)i]] : 0;
        px[i] = (thermalSingle && ch[0] == ch[1] && ch[1] == ch[2] && s[0] && s[0]->px[(size_t)i]) ? meteorThermal((uint8_t)r) : (0xFF000000u | b << 16 | g << 8 | r);
    }
}

} // namespace dect2
