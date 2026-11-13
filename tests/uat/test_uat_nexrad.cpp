// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FIS-B NEXRAD blocks: RLE round trip, geographic placement, empty-block bitmap, garbage. Simulated data only.
#include "dect2/uat/uat_nexrad.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    std::mt19937 g(1);
    for (int t = 0; t < 200; t++) {
        UatNexradBlock b;
        b.blockNum = g() % 100000;
        int run = 0, v = 0;
        for (int i = 0; i < 128; i++) { if (!run--) { run = (int)(g() % 40); v = (int)(g() % 8); } b.bins[i] = (uint8_t)v; }
        std::vector<uint8_t> d;
        uatNexradEncodeRle(b, 0, false, d);
        std::vector<UatNexradBlock> out;
        CHECK(uatNexradDecode(63, d.data(), d.size(), out));
        CHECK(out.size() == 1 && out[0].bins == b.bins && out[0].blockNum == b.blockNum);
    }
    // block 450 * 30 + 12: 2 degrees north, 9.6 degrees east
    { UatNexradBlock b; b.blockNum = 450 * 30 + 12; std::vector<uint8_t> d; uatNexradEncodeRle(b, 0, false, d); std::vector<UatNexradBlock> o; CHECK(uatNexradDecode(64, d.data(), d.size(), o));
      CHECK(std::fabs(o[0].latNorth - (30 * 4 + 4) / 60.0) < 1e-9 && std::fabs(o[0].lonWest - 12 * 0.8) < 1e-9 && std::fabs(o[0].latSize - 4 / 60.0) < 1e-9); }
    // scale 1: 5 x block size, wide blocks above 60 degrees
    { UatNexradBlock b; b.blockNum = 405001; std::vector<uint8_t> d; uatNexradEncodeRle(b, 1, true, d); std::vector<UatNexradBlock> o; CHECK(uatNexradDecode(63, d.data(), d.size(), o));
      CHECK(o[0].blockNum == 405000 && std::fabs(o[0].lonSize - 8.0) < 1e-9 && o[0].latNorth < 0); }
    // empty bitmap: block b, b+1 (low nibble flag), and one in the extra byte
    { uint8_t d[6] = {0x00, 0x00, 0x64, (uint8_t)(0x10 | 1), 0x08};
      std::vector<UatNexradBlock> o; CHECK(uatNexradDecode(64, d, 5, o)); CHECK(o.size() == 3 && o[0].blockNum == 100 && o[1].blockNum == 101 && o[2].blockNum == 100 + 8 + 3 - 3); CHECK(o[0].bins[50] == 1); }
    // truncated RLE and wrong product
    { uint8_t d[4] = {0x80, 0, 0, 0xFF}; std::vector<UatNexradBlock> o; CHECK(!uatNexradDecode(63, d, 4, o)); CHECK(!uatNexradDecode(8, d, 4, o)); }
    return testSummary("uat_nexrad");
}
