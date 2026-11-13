// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FPV channel plans: the tables, names and the nearest-channel lookup.
#include "dect2/fpv/fpv_chan.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const auto all = fpvAllChannels();
    CHECK(all.size() == (size_t)kFpvBandCount * 8, "%zu channels", all.size());
    CHECK(kFpvBandCount == 7, "%d bands", kFpvBandCount);
    // well-known entries
    auto freq = [&](const char* n) { for (const auto& c : all) if (c.name == n) return c.freqHz / 1e6; return -1.0; };
    CHECK(freq("R1") == 5658 && freq("R5") == 5806 && freq("R8") == 5917, "Raceband");
    CHECK(freq("A1") == 5865 && freq("A8") == 5725, "band A");
    CHECK(freq("B1") == 5733 && freq("B8") == 5866, "band B");
    CHECK(freq("E1") == 5705 && freq("E5") == 5885 && freq("E8") == 5945, "band E");
    CHECK(freq("F1") == 5740 && freq("F8") == 5880, "band F");
    CHECK(freq("L1") == 5362 && freq("L8") == 5621, "band L");
    CHECK(freq("G1") == 1080 && freq("G8") == 1360, "1.2 GHz");
    // Raceband steps by 37 MHz, F and A/B by 20 MHz (except the ends)
    for (int c = 1; c < 8; c++) CHECK(std::fabs(kFpvBands[4].mhz[c] - kFpvBands[4].mhz[c - 1] - 37) < 1.01, "R%d step", c + 1);
    for (int c = 1; c < 8; c++) CHECK(kFpvBands[3].mhz[c] - kFpvBands[3].mhz[c - 1] == 20, "F%d step", c + 1);
    // names are unique
    for (size_t i = 0; i < all.size(); i++) for (size_t j = i + 1; j < all.size(); j++) CHECK(all[i].name != all[j].name, "duplicate name %s", all[i].name.c_str());
    // the nearest channel
    int i = fpvNearest(all, 5806.4e6, 4e6);
    CHECK(i >= 0 && all[(size_t)i].name == "R5", "nearest to 5806.4: %s", i >= 0 ? all[(size_t)i].name.c_str() : "none");
    CHECK(fpvNearest(all, 3000e6, 4e6) == -1, "nothing near 3000 MHz");
    i = fpvNearest(all, 1200.5e6, 4e6);
    CHECK(i >= 0 && all[(size_t)i].name == "G4", "1.2 GHz channel");
    // the neighbours that share a frequency
    const std::string n = fpvNamesNear(all, 5806e6, 1.5e6);
    CHECK(n.find("R5") != std::string::npos && n.find("A4") != std::string::npos, "names near 5806: %s", n.c_str());
    return testSummary("fpv chan");
}
