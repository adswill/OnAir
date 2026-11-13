// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// UMTS cell search on simulated downlink signals: several cells, frequency offset, noise, the whole set of code groups.
#include "dect2/umts/umts_cell.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    struct T { std::vector<UmtsTxCell> cells; double snr; double maxCfo; };
    std::vector<T> ts = {
        {{{0, 0, 1, 137, 0}}, 30, 2500}, {{{511, 1800, 1, 900, 0.5}}, 10, 7500}, {{{97, -4200, 1, 3000, 0.4}}, 3, 7500},
        {{{12, 0, 1, 500, 0.3}, {200, 2100, 0.8, 7000, 0.3}, {401, -3000, 0.7, 22000, 0.3}}, 15, 7500},
    };
    for (int g = 0; g < 64; g += 9) ts.push_back({{{g * 8 + 3, 1000.0 * (g % 3 - 1), 1, 1000 + 500 * g, 0.5}}, 8, 5000});
    int idx = 0;
    for (auto& t : ts) {
        auto x = umtsGenerate(t.cells, 38400 * 2 * 8, t.snr, 5 + idx++);
        double ms = 0;
        auto f = umtsCellSearch(x.data(), x.size(), t.maxCfo, 6, &ms);
        printf("%zu cells @ %.0f dB: found %zu in %.0f ms:", t.cells.size(), t.snr, f.size(), ms);
        for (auto& c : f) printf(" PSC %d (Ec/Io %.1f dB, cfo %.0f)", c.scramblingCode, c.ecIoDb, c.cfoHz);
        printf("\n");
        for (auto& c : t.cells) { bool ok = false; for (auto& r : f) if (r.scramblingCode == c.code && std::abs(r.cfoHz - c.cfoHz) <= 1400) ok = true; CHECK(ok, "cell %d not found (snr %.0f)", c.code, t.snr); }
    }
    {   // no cell: nothing reported
        auto n = umtsGenerate({}, 38400 * 2 * 8, 150, 9);
        for (auto& v : n) v = cf32(0.3f * ((float)rand() / RAND_MAX - 0.5f), 0.3f * ((float)rand() / RAND_MAX - 0.5f));
        auto f = umtsCellSearch(n.data(), n.size(), 7500, 6);
        CHECK(f.empty(), "%zu false cells on noise", f.size());
    }
    {   // the broadcast channel: operator and cell identity through the whole chain, at a realistic Ec/Io
        for (double snr : {25.0, 5.0}) for (double cfo : {0.0, 2100.0}) {
            auto x = umtsGenerate({{33, cfo, 1, 1700, 0.3, 246, 3, 2, 0x0ABCDEF}}, 38400 * 2 * 8, snr, 11);
            auto f = umtsCellSearch(x.data(), x.size(), 7500, 6);
            bool ok = false; for (auto& c : f) if (c.scramblingCode == 33) { printf("BCH snr %.0f cfo %.0f: blocks %d, MCC %d MNC %d, cell %lld\n", snr, cfo, c.bchBlocks, c.mcc, c.mnc, (long long)c.cellIdentity); if (c.mcc == 246 && c.mnc == 3 && c.cellIdentity == 0x0ABCDEF && c.bchBlocks >= 2) ok = true; }
            CHECK(ok, "BCH snr %.0f cfo %.0f", snr, cfo);
        }
    }
    // the radio's sample clock off (QA sweep 3.9: the chips moved through the samples, and at 5 ppm no cell was found): -50 .. +50 ppm
    for (double ppm : {-50.0, -15.0, -5.0, 5.0, 30.0}) {
        auto x0 = umtsGenerate({{17, -2000, 1, 500, 0.4, 1, 1, 2, 0x123456}, {280, 300, 0.7, 9000, 0.3, 1, 1, 2, 0x234567}}, 38400 * 2 * 8 + 2000, 30, 13);
        std::vector<cf32> x;
        for (double t = 1; t + 2 < (double)x0.size() && x.size() < 38400 * 2 * 8; t += 1 + ppm * 1e-6) {
            const size_t i = (size_t)t; const float fr = (float)(t - (double)i);
            x.push_back(x0[i] * (1 - fr) + x0[i + 1] * fr);
        }
        auto f = umtsCellSearch(x.data(), x.size(), 7500, 6);
        int cells = 0, bch = 0;
        for (auto& c : f) { if (c.scramblingCode == 17 || c.scramblingCode == 280) cells++; bch += c.bchBlocks; }
        printf("  clock %+.0f ppm: %d cells, %d BCH blocks\n", ppm, cells, bch);
        CHECK(cells == 2 && bch >= 3, "clock %.0f ppm: %d cells, %d BCH", ppm, cells, bch);
    }
    printf(fails ? "umts cells FAILED\n" : "umts cells passed\n");
    return fails ? 1 : 0;
}
