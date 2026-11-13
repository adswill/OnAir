// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers, the band scan's search of one 20 Msps step: several 5 MHz carriers side by side (no gap in the spectrum between them),
// each with two or three cells 3-10 dB apart, synchronised (one site's sectors, and two cells of the same NID2) or not (other sites).
// Every cell must be listed, the ones 6 dB or less under the strongest with their MIB; noise alone must list nothing.
#include "dect2/cell/cell_dsp.h"
#include "dect2/common/stable_random.h"
#include "dect2/cell/cell_gen.h"
#include "dect2/cell/cell_lte.h"
#include "dect2/cell/cell_rx.h"
#include "dect2/cell/cell_tel.h"
#include "../common/impair.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
using namespace dect2;
using namespace dect2::cell;
#include "../common/test_util.h"

constexpr double kFs = 20e6, kCenter = 800e6;
constexpr int kFrames = 26;

struct Cell { double offHz; int pci; float db; double delayMs; bool needMib; };

// one cell at 20 Msps: the 5 MHz test cell with its own PCI and data, its frame start delayed by delayMs
static std::vector<cf32> oneCell(const Cell& c, uint32_t seed) {
    LteGenCfg g = cellTestCell(1, nullptr, c.pci);
    g.seed = seed;
    g.sfn0 = 100 + (int)seed;
    const std::vector<cf32> x = lteGenerate(g, kFrames + 1);
    const double f0 = lteRate(lteNfft(g.cell.nRb));
    const size_t n = (size_t)(kFrames * 0.01 * kFs);
    std::vector<cf32> y = resampleBlock(x.data(), x.size(), f0, kFs, 0, f0 * 0.95, c.delayMs * 1e-3 * f0, n);
    impair::shift(y, c.offHz, kFs);
    const float a = std::pow(10.f, c.db / 20);
    for (auto& v : y) v *= a;
    return y;
}

static CellTelemetry run(const std::vector<cf32>& x) {
    CellReceiver rx;
    rx.configure(kFs);
    rx.setCenterHz(kCenter);
    const size_t chunk = (size_t)(kFs * 0.01);
    for (size_t i = 0; i < x.size(); i += chunk) {
        rx.feed(&x[i], std::min(chunk, x.size() - i));
        while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::vector<cf32> z(chunk, cf32(0, 0));
    rx.feed(z.data(), 1);
    CellTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

int main() {
    // three 5 MHz carriers edge to edge, 795 / 800 / 805 MHz
    const Cell cells[] = {
        {-5e6, 200, 0, 0, true},      // site A, three sectors: synchronised; 203 has the NID2 of 200 (same PSS, told apart by the SSS)
        {-5e6, 201, -3, 0, true},
        {-5e6, 203, -6, 0, false},   // its PBCH lies under the PBCH of 200 (same timing), so only its PSS / SSS are required
        {0, 17, -2, 1.7, true},       // another site, not synchronised with A
        {0, 30, -10, 3.1, false},     // a far cell 8 dB under it, other timing
        {5e6, 300, -1, 0.4, true},
        {5e6, 302, -7, 0.4, false},   // a sector of the same site 6 dB down, synchronised
    };
    std::vector<cf32> x;
    uint32_t seed = 1;
    for (const Cell& c : cells) {
        const std::vector<cf32> y = oneCell(c, seed++);
        if (x.empty()) x.assign(y.size(), cf32(0, 0));
        for (size_t i = 0; i < x.size() && i < y.size(); i++) x[i] += y[i];
    }
    impair::noise(x, 15, 21);
    const auto t0 = std::chrono::steady_clock::now();
    const CellTelemetry t = run(x);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("  %zu cells listed from %llu blocks (%.0f ms)\n", t.cells.size(), (unsigned long long)t.blocks, ms);
    for (const auto& c : t.cells) printf("    PCI %3d %.1f MHz mib %d %d RB, RSRP %.1f\n", c.pci, c.freqHz / 1e6, c.hasMib, c.nRb, c.rsrpDb);
    for (const Cell& c : cells) {
        const CellTower* f = nullptr;
        for (const auto& k : t.cells) if (k.pci == c.pci) f = &k;
        CHECK(f, "PCI %d (%.0f dB at %+.0f MHz) not listed", c.pci, c.db, c.offHz / 1e6);
        if (!f) continue;
        CHECK(std::fabs(f->freqHz - (kCenter + c.offHz)) < 1e3, "PCI %d at %.3f MHz", c.pci, f->freqHz / 1e6);
        if (c.needMib) CHECK(f->hasMib && f->nRb == 25, "PCI %d: mib %d %d RB", c.pci, f->hasMib, f->nRb);
    }
    for (const auto& k : t.cells) {
        bool known = false;
        for (const Cell& c : cells) if (k.pci == c.pci) known = true;
        CHECK(known, "a cell that is not there: PCI %d at %.1f MHz", k.pci, k.freqHz / 1e6);
    }
    {   // noise alone through the whole receiver (spectrum, raster probe, search): nothing
        std::vector<cf32> n(x.size());
        std::mt19937 rng(5);
        dect2::normal_distribution<float> nd(0.f, 0.1f);
        for (auto& v : n) v = cf32{nd(rng), nd(rng)};
        const CellTelemetry tn = run(n);
        CHECK(tn.cells.empty(), "noise only listed %zu cells", tn.cells.size());
    }
    printf(fails ? "cell scan: %d FAILED\n" : "cell scan: all passed\n", fails);
    return fails ? 1 : 0;
}
