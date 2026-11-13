// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HD Radio map tiles: names, the 3 x 3 composite from out-of-order tiles, newer sets replacing older ones, radar names and position.
#include "dect2/hdr/hdr_maps.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dect2::hdr;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
int main() {
    int x, y; std::string st, id;
    CHECK(parseTrafficName("TMT_0123_2_3_20261109_2130_Abc.png", x, y, st) && x == 1 && y == 2 && st == "20261109_2130", "traffic name");
    CHECK(!parseTrafficName("TMT_a_9_3_20261109_2130.png", x, y, st) && !parseTrafficName("OTHER.png", x, y, st), "not traffic");
    CHECK(parseRadarName("DWRO_ABCD_1_20261109_2130_xx.png", id, st) && id == "ABCD" && st == "20261109_2130", "radar name %s", id.c_str());
    double c[4]; CHECK(parseRadarInfo("Foo\r\nDWR_Area_ID=\"KATL\"\r\nCoordinates=(34.5,-85.2)(33.1,-83.7)\r\n", id, c) && id == "KATL" && c[0] == 34.5 && c[3] == -83.7, "info %s", id.c_str());
    std::vector<uint8_t> px(200 * 200 * 4);
    TrafficMap m;
    int order[9] = {4, 0, 8, 3, 7, 1, 6, 5, 2}, done = 0;
    for (int i : order) { int tx = i / 3, ty = i % 3; std::fill(px.begin(), px.end(), (uint8_t)(10 * (tx + 1) + ty + 1)); done += m.addTile(tx, ty, "20261109_2130", px.data(), 200, 200); CHECK(done == (i == order[8] ? 1 : 0), "ready too early"); }
    CHECK(m.ready() && m.image().w == 600 && m.image().stamp == "20261109_2130", "map");
    if (m.ready()) { auto pxAt = [&](int r, int c) { return m.image().rgba[(size_t)r * 600 * 4 + (size_t)c * 4]; }; CHECK(pxAt(10, 10) == 11, "tile 0,0"); CHECK(pxAt(10, 410) == 13, "row 0 column 2: %d", pxAt(10, 410)); CHECK(pxAt(410, 10) == 31, "row 2 column 0"); CHECK(pxAt(599, 599) == 33, "tile 2,2"); }
    std::fill(px.begin(), px.end(), 99); CHECK(!m.addTile(0, 0, "20261109_2145", px.data(), 200, 200) && m.ready() && m.tilesOfCurrent() == 1, "new set in progress");
    CHECK(!m.addTile(0, 0, "x", px.data(), 100, 200), "wrong size refused");
    std::vector<uint8_t> o = {0, 0, 0, 0, 200, 100, 0, 255}; MapImage im; radarComposite(o.data(), 2, 1, im);
    CHECK(im.rgba[0] == 40 && im.rgba[3] == 255 && im.rgba[4] == 200, "radar over background");
    printf(fails ? "hdr maps FAILED\n" : "hdr maps passed\n");
    return fails ? 1 : 0;
}
