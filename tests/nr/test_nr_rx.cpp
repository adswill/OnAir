// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 5G NR cells search: synthetic SS/PBCH bursts (38.211 / 38.212) through searchDecode at other sample rates, off the centre, with carrier
// and clock offsets, noise and weak beams; noise alone finds nothing.
#include "dect2/cell/cell_dsp.h"
#include "dect2/common/stable_random.h"
#include "dect2/nr/nr_phy.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::nr;
#include "../common/test_util.h"

struct Placed { GenCell g; double offHz; float db; };

// the cells at their offsets, at rate fs, carrier offset cfo, clock error ppm, noise for snrDb against a 0 dB cell inside its SSB
static std::vector<cf32> scene(const std::vector<Placed>& cells, double fs, double cfo, double ppm, double snrDb, double sec, uint32_t seed) {
    const size_t n = (size_t)(fs * sec);
    std::vector<cf32> out(n, cf32(0, 0));
    for (const auto& c : cells) {
        const double nat = 256e3 * c.g.scsKhz;
        const int nh = (int)std::ceil(sec / 0.005) + 2;
        const std::vector<cf32> s = generate(c.g, nh);
        const std::vector<cf32> r = cell::resampleBlock(s.data(), s.size(), nat, fs, 0, nat * 0.97, 300.0, n, 1.0 + ppm * 1e-6);
        const float a = std::pow(10.f, c.db / 20);
        const double w = 2 * M_PI * (c.offHz + cfo) / fs;
        for (size_t i = 0; i < n; i++) out[i] += r[i] * a * cf32((float)std::cos(w * (double)i), (float)std::sin(w * (double)i));
    }
    if (snrDb < 99) {
        const double nat = 256e3 * (cells.empty() ? 30 : cells[0].g.scsKhz);
        const float sd = (float)std::sqrt(0.04 * std::max(1.0, fs / nat) / std::pow(10.0, snrDb / 10) / 2);
        std::mt19937 rng(seed);
        dect2::normal_distribution<float> nd(0, 1);
        for (auto& v : out) v += cf32{sd * nd(rng), sd * nd(rng)};
    }
    return out;
}

static GenCell mkCell(int pci, int scs, int lmax, int sfn, bool hrf = false) {
    GenCell g;
    g.pci = pci; g.scsKhz = scs; g.lmax = lmax; g.ssbMask = lmax == 8 ? 0xFF : 0xF;
    g.mib.sfn = sfn; g.mib.hrf = hrf; g.mib.scs30 = scs == 30; g.mib.kSsb = 7; g.mib.coreset0 = 10; g.mib.ss0 = 3; g.mib.dmrsTypeAPos = 3;
    g.seed = 9 + (uint32_t)pci;
    return g;
}

static const CellResult* findPci(const std::vector<CellResult>& r, int pci) { for (const auto& c : r) if (c.pci == pci) return &c; return nullptr; }

int main() {
    using clk = std::chrono::steady_clock;
    // 1: a 30 kHz cell, 8 beams, at its native 7.68 Msps
    {
        auto x = scene({{mkCell(333, 30, 8, 100), 0, 0}}, 7.68e6, 0, 0, 15, 0.080, 1);
        SearchOpts o; o.scsKhz = 30;
        const auto t0 = clk::now();
        auto r = searchDecode(x.data(), x.size(), 7.68e6, 0, o);
        printf("  native 30 kHz: %zu cells, %.0f ms\n", r.size(), std::chrono::duration<double, std::milli>(clk::now() - t0).count());
        const CellResult* c = findPci(r, 333);
        CHECK(c && c->hasMib, "cell 333 MIB");
        if (c && c->hasMib) {
            CHECK(c->mib.sfn >= 100 && c->mib.sfn <= 104 && c->mib.kSsb == 7 && c->mib.coreset0 == 10 && c->mib.ss0 == 3 && c->mib.dmrsTypeAPos == 3 && c->mib.scs30, "MIB fields sfn %d", c->mib.sfn);
            CHECK(c->beams.size() == 8, "beams %zu", c->beams.size());
            CHECK(c->sinrDb > 8 && c->rsrqDb < -10 && c->rsrqDb > -16, "SINR %.1f RSRQ %.1f", c->sinrDb, c->rsrqDb);
            printf("  RSRP %.1f dBFS RSRQ %.1f dB SINR %.1f dB, cfo %.0f Hz\n", c->rsrpDb, c->rsrqDb, c->sinrDb, c->cfoHz);
        }
        for (const auto& q : r) printf("  found PCI %d mib %d sig %.1f pss %.1f\n", q.pci, q.hasMib, q.sssSig, q.pssDb);
        CHECK(r.size() == 1, "one cell, %zu", r.size());
    }
    // 2: two cells at 20 Msps, +-4.32 MHz from the centre, +23 kHz carrier offset, 10 ppm clock, 0 dB SNR
    {
        GenCell b = mkCell(1007, 30, 8, 1020);   // the SFN wraps through 1023 -> 0
        b.beamDb = {-12, -9, -6, -3, 0, -3, -6, -9};
        auto x = scene({{mkCell(4, 30, 8, 7), -4.32e6, 0}, {b, 4.32e6, -3}}, 20e6, 23e3, 10, 0, 0.080, 2);
        SearchOpts o; o.scsKhz = 30; o.maxCfoHz = 35e3;
        for (double off : {-4.32e6, 4.32e6}) {
            const auto t0 = clk::now();
            auto r = searchDecode(x.data(), x.size(), 20e6, off, o);
            const int want = off < 0 ? 4 : 1007;
            const CellResult* c = findPci(r, want);
            printf("  20 Msps at %+.2f MHz: %zu cells, %.0f ms, cfo %.0f Hz\n", off / 1e6, r.size(), std::chrono::duration<double, std::milli>(clk::now() - t0).count(), c ? c->cfoHz : 0.0);
            CHECK(c && c->hasMib, "cell %d MIB", want);
            if (c) CHECK(std::fabs(c->cfoHz - 23e3) < 800, "cfo %.0f", c->cfoHz);   // under 0.03 subcarrier at -3 dB
            if (c && want == 1007 && c->hasMib) CHECK(c->mib.ssbIdx == 4 && c->beams.size() >= 5, "strongest beam %d, %zu beams", c->mib.ssbIdx, c->beams.size());
            CHECK(r.size() == 1, "only the cell at the offset: %zu", r.size());
        }
    }
    // 3: a 15 kHz Lmax-4 cell starting in its second half frame with a 5 ms SSB period (SSB DMRS index 4..7 every other burst), 10 Msps,
    //    -9 kHz, 3 dB
    {
        GenCell g = mkCell(222, 15, 4, 300, true);
        g.periodMs = 5;
        auto x = scene({{g, 1.2e6, 0}}, 10e6, -9e3, -5, 3, 0.080, 3);
        SearchOpts o; o.scsKhz = 15; o.maxCfoHz = 15e3;
        auto r = searchDecode(x.data(), x.size(), 10e6, 1.2e6, o);
        const CellResult* c = findPci(r, 222);
        CHECK(c && c->hasMib, "15 kHz cell MIB");
        if (c && c->hasMib) {
            printf("  15 kHz Lmax 4: SSB %d hrf %d lmax %d DMRS %d, %zu beams, cfo %.0f\n", c->mib.ssbIdx, c->mib.hrf, c->mib.lmax, c->issbBar, c->beams.size(), c->cfoHz);
            CHECK(c->mib.ssbIdx >= 0 && c->mib.ssbIdx < 4 && c->beams.size() == 4 && !c->mib.scs30, "beams %zu", c->beams.size());
            CHECK(c->issbBar < 4 || (c->mib.lmax == 4 && c->mib.hrf), "Lmax 4 second half frame");
        }
        // the wrong spacing finds nothing
        SearchOpts o30; o30.scsKhz = 30;
        auto r30 = searchDecode(x.data(), x.size(), 10e6, 1.2e6, o30);
        CHECK(r30.empty() || !r30[0].hasMib, "30 kHz search of a 15 kHz cell");
    }
    // 4: weak: -5 dB SNR (four bursts folded for the PSS)
    {
        auto x = scene({{mkCell(600, 30, 8, 44), 0, 0}}, 7.68e6, 4e3, 0, -5, 0.080, 4);
        SearchOpts o; o.scsKhz = 30;
        auto r = searchDecode(x.data(), x.size(), 7.68e6, 0, o);
        const CellResult* c = findPci(r, 600);
        printf("  -5 dB: %s, SINR %.1f\n", c ? (c->hasMib ? "MIB" : "PCI only") : "nothing", c ? c->sinrDb : 0.f);
        CHECK(c && c->hasMib, "weak cell");
    }
    // 5: noise alone (and a CW tone) gives nothing
    {
        int falseCells = 0;
        for (uint32_t s = 0; s < 4; s++) {
            auto x = scene({}, 20e6, 0, 0, 0, 0.080, 100 + s);
            for (size_t i = 0; i < x.size(); i++) x[i] += cf32(0.05f * (float)std::cos(0.1 * i), 0.05f * (float)std::sin(0.1 * i));
            for (int scs : {30, 15}) {
                SearchOpts o; o.scsKhz = scs;
                for (const auto& c : searchDecode(x.data(), x.size(), 20e6, 0.3e6 * s, o)) if (c.hasMib || c.sssSig >= 6) falseCells++;
            }
        }
        CHECK(falseCells == 0, "noise gave %d cells", falseCells);
    }
    printf(fails ? "nr rx: %d FAILED\n" : "nr rx: all passed\n", fails);
    return fails ? 1 : 0;
}
