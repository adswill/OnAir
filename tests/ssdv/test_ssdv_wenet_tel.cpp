// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wenet telemetry packets built by hand to the published layouts (simulated): GPS, system and power blocks, orientation, text, wrong types and padding.
#include "dect2/ssdv/wenet_tel.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static void pf(uint8_t* p, float f) { uint32_t v; memcpy(&v, &f, 4); p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
int main() {
    uint8_t p[256]; memset(p, 0x55, 256);
    p[0] = 1; p[1] = 0x09; p[2] = 0x2A; uint32_t tow = 123456789; p[3] = tow >> 24; p[4] = tow >> 16; p[5] = tow >> 8; p[6] = tow; p[7] = 18;
    pf(p + 8, -34.9f); pf(p + 12, 138.5f); pf(p + 16, 28000.0f); pf(p + 20, 12.5f); pf(p + 24, 270.0f); pf(p + 28, 5.5f); p[32] = 9; p[33] = 3; p[34] = 6;
    WenetGps g;
    CHECK(wenetParseGps(p, 256, g) && g.valid, "gps");
    CHECK(g.week == 2346 && g.leap == 18 && std::abs(g.iTow - 123456.789) < 0.001, "time");
    CHECK(std::abs(g.lat + 34.9) < 1e-3 && std::abs(g.alt - 28000) < 1 && g.numSv == 9 && g.fix == 3, "fields");
    CHECK(!g.haveSys && !g.havePower, "padding means absent");
    pf(p + 35, 41.5f); pf(p + 39, 50.0f); p[43] = 0x07; p[44] = 0xD0;
    p[73] = 0x10; p[74] = 0x68; p[75] = 0; p[76] = 200; pf(p + 77, 12.0f);
    CHECK(wenetParseGps(p, 256, g) && g.haveSys && g.havePower && g.cpuSpeed == 2000 && std::abs(g.battV - 4.2) < 1e-3 && g.battI == 200, "system blocks");
    p[0] = 2; p[8] = 1; p[14] = 0xFB; pf(p + 15, 90.0f); pf(p + 19, -10.0f); pf(p + 23, 5.0f); WenetOrient o;
    CHECK(wenetParseOrient(p, 256, o) && o.temp == -5 && std::abs(o.heading - 90) < 1e-3 && std::abs(o.roll + 10) < 1e-3, "orientation");
    memset(p, 0x55, 256); p[0] = 0; p[1] = 5; p[2] = 0x12; p[3] = 0x34; memcpy(p + 4, "HELLO", 5); WenetText t;
    CHECK(wenetParseText(p, 256, t) && t.text == "HELLO" && t.id == 0x1234, "text");
    p[1] = 255; CHECK(!wenetParseText(p, 10, t), "text too long");
    p[0] = 9; CHECK(!wenetParseGps(p, 256, g) && !wenetParseOrient(p, 256, o) && !wenetParseText(p, 256, t), "wrong type");
    printf(fails ? "wenet FAILED\n" : "wenet passed\n");
    return fails ? 1 : 0;
}
