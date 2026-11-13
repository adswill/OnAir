// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The map colour presets: valid values, readable labels on every sea colour, today's look unchanged, overlay helpers legible.
#include "map/map_colours.h"
#include <cstdio>
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    using namespace mapcol;
    CHECK(kPresetCount >= 6, "at least six presets");
    for (int i = 0; i < kPresetCount; i++) {
        const Preset& p = kPresets[i];
        CHECK(p.name && p.name[0], "preset %d has a name", i);
        for (uint32_t c : {p.s.sea, p.s.grid, p.s.coast, p.s.label, p.s.tint}) CHECK(c <= 0xFFFFFF, "%s: colour out of range", p.name);
        CHECK(ratio(p.s.label, p.s.sea) >= kMinLabelRatio, "%s: label/sea ratio %.2f", p.name, ratio(p.s.label, p.s.sea));
        CHECK(ratio(p.s.coast, p.s.sea) >= 1.5f, "%s: coast invisible on sea", p.name);
        CHECK(ratio(textOn(p.s.sea), p.s.sea) >= 7.f, "%s: overlay text ratio %.2f", p.name, ratio(textOn(p.s.sea), p.s.sea));
        for (uint32_t m : {0xFFC83Cu, 0x50AAFFu, 0x78E678u, 0xFF78C8u, 0xF05A50u, 0x6EF2E6u, 0xFFFFFFu, 0x000000u})
            CHECK(ratio(legibleOn(m, p.s.sea), p.s.sea) >= 3.0f, "%s: marker %06X not legible (%.2f)", p.name, m, ratio(legibleOn(m, p.s.sea), p.s.sea));
    }
    const Scheme& d = kPresets[kDefault].s;   // today's look: the colours adsb_map.cpp drew before the schemes existed
    CHECK(d.sea == 0x0E1012 && d.grid == 0x1E2226 && d.coast == 0x3A4A42 && d.tint == 0x78808C, "default scheme changed");
    CHECK(d.label == 0x969A9E, "default label changed");
    CHECK(pack(unpack(0x12AB34)) == 0x12AB34, "pack/unpack round trip");
    CHECK(lightBg(0xDCE6EE) && !lightBg(0x0E1012), "lightBg");
    CHECK(legibleOn(0xFFC83C, 0x0E1012) == 0xFFC83C, "a readable colour is left alone");
    if (!failures) std::printf("map colours OK\n");
    return failures ? 1 : 0;
}
