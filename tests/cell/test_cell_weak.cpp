// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers, weak co-channel cells: one 5 MHz carrier with the sectors of a synchronised network (same frame timing, same SFN), 3 to 9
// dB under the strongest, one of them sharing its NID2 (the same PSS). Each must be read to its SIB1 (operator and cell identity): the MIB
// combined over the 40 ms periods, or borrowed from the strong cell, the strong cell's reference signals and PBCH cancelled from under it,
// SIB1 combined over its repetitions. Noise alone must give nothing.
#include "dect2/cell/cell_dsp.h"
#include "dect2/common/stable_random.h"
#include "dect2/cell/cell_gen.h"
#include "dect2/cell/cell_lte.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::cell;
#include "../common/test_util.h"

constexpr double kFs = 7.68e6;
constexpr int kFrames = 20;

struct Cell { int pci; float db; };

static std::vector<cf32> oneCell(const Cell& c, uint32_t seed) {
    LteGenCfg g = cellTestCell(1, nullptr, c.pci);
    g.seed = seed;
    g.sfn0 = 300;   // one network: every cell has the same SFN at the same time
    std::vector<cf32> y = lteGenerate(g, kFrames);
    const float a = std::pow(10.f, c.db / 20);
    for (auto& v : y) v *= a;
    return y;
}

static bool hasSib1(const LteCellResult& r) {
    for (const auto& s : r.si) if (s.subframe == 5) return true;
    return false;
}

int main() {
    const Cell cells[] = {
        {120, 0},
        {121, -3},
        {123, -6},     // the NID2 of 120: under its PSS
        {122, -9},
    };
    std::vector<cf32> x;
    uint32_t seed = 7;
    for (const Cell& c : cells) {
        const std::vector<cf32> y = oneCell(c, seed++);
        if (x.empty()) x.assign(y.size(), cf32(0, 0));
        for (size_t i = 0; i < x.size() && i < y.size(); i++) x[i] += y[i];
    }
    impair::shift(x, 700, kFs);
    impair::noise(x, 25, 3);
    LteSearchOpts o;
    const auto res = lteSearchDecode(x.data(), x.size(), kFs, 0, o);
    for (const auto& r : res) printf("  PCI %3d mib %d %d RB %d ports, %zu SI, SIB1 %d\n", r.found.pci, r.mib.ok, r.mib.nRb, r.mib.ports, r.si.size(), hasSib1(r));
    for (const Cell& c : cells) {
        const LteCellResult* f = nullptr;
        for (const auto& r : res) if (r.found.pci == c.pci) f = &r;
        CHECK(f, "PCI %d (%.0f dB) not found", c.pci, c.db);
        if (!f) continue;
        CHECK(f->mib.ok && f->mib.nRb == 25, "PCI %d (%.0f dB): mib %d %d RB", c.pci, c.db, f->mib.ok, f->mib.nRb);
        CHECK(hasSib1(*f), "PCI %d (%.0f dB): no SIB1", c.pci, c.db);
    }
    for (const auto& r : res) {
        bool known = false;
        for (const Cell& c : cells) if (r.found.pci == c.pci) known = true;
        CHECK(known, "a cell that is not there: PCI %d", r.found.pci);
    }
    {
        std::vector<cf32> n(x.size());
        std::mt19937 rng(11);
        dect2::normal_distribution<float> nd(0.f, 0.2f);
        for (auto& v : n) v = cf32{nd(rng), nd(rng)};
        const auto rn = lteSearchDecode(n.data(), n.size(), kFs, 0, o);
        CHECK(rn.empty(), "noise only gave %zu cells", rn.size());
    }
    printf(fails ? "cell weak: %d FAILED\n" : "cell weak: all passed\n", fails);
    return fails ? 1 : 0;
}
