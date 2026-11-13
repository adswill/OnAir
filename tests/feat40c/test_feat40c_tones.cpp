// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 4.0 Lane B group 3: tone system identification (links the tone tables of dect2core). Simulated tone lists only.
#include "dect2/tones/tone_ident.h"
#include <cstdio>
#include <random>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
using namespace dect2;
int main() {
    std::mt19937 rng(2); std::uniform_real_distribution<double> j(-3, 3);
    // ZVEI1 call 12345 at 70 ms per tone, with 3 Hz of frequency error
    auto seq = [&](const char* id, const std::vector<int>& digits, double len) {
        for (int k = 0; k < tones::kSeqSetN; k++) if (std::string(tones::kSeqSets[k].id) == id) {
            std::vector<HeardTone> t; double st = 0;
            for (int d : digits) { t.push_back({tones::kSeqSets[k].digit[d] + j(rng), len, st}); st += len + 5; }
            return t;
        }
        return std::vector<HeardTone>();
    };
    auto g = identifyToneSystem(seq("zvei1", {1, 2, 3, 4, 5}, 70));
    CHECK(!g.empty() && (g[0].system == "zvei1" || g[0].system == "zvei2" || g[0].system == "dzvei"));
    bool z1 = false; for (auto& x : g) if (x.system == "zvei1") z1 = true; CHECK(z1);
    g = identifyToneSystem(seq("eia", {1, 2, 3, 4, 5}, 33));
    CHECK(!g.empty() && g[0].system == "eia");
    // CCIR (100 ms) beats EEA (40 ms) on the tone length
    g = identifyToneSystem(seq("ccir", {7, 7, 3, 1, 0}, 100)); CHECK(!g.empty() && g[0].system == "ccir");
    g = identifyToneSystem(seq("eea", {7, 7, 3, 1, 0}, 40)); CHECK(!g.empty() && g[0].system == "eea");
    // DTMF key 5: 770 + 1336 together
    g = identifyToneSystem({{771, 80, 0}, {1337, 80, 0}, {697, 80, 150}, {1209, 80, 150}});
    CHECK(!g.empty() && g[0].system == "DTMF");
    // CTCSS 88.5
    g = identifyToneSystem({{88.5, 3000, 0}}); CHECK(!g.empty() && g[0].system == "CTCSS" && g[0].detail.find("88.5") != std::string::npos);
    // a steady 1000 Hz and random tones are not a system
    g = identifyToneSystem({{1000, 3000, 0}}); CHECK(g.empty());
    g = identifyToneSystem({{1234, 70, 0}, {2345, 70, 80}, {777, 70, 160}, {1500, 70, 240}}); CHECK(g.empty());
    printf(fails ? "feat40c_tones: %d FAILED\n" : "feat40c_tones: all passed\n", fails);
    return fails != 0;
}
