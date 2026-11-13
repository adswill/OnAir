// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pilot-map invariants. The full 798-configuration comparison against the reference transmitter was run offline;
// these are spot checks of the numbers it produced.
#include "dect2/t2/t2pilots.h"
#include <cstdio>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
int main() {
    struct G { int fft, ext, pp, cp2, cdata, nfc; } g[] = {
        {1, 0, 0, 4472, 6208, 4544}, {1, 0, 6, 4472, 6698, 6532}, {1, 1, 0, 4472, 6296, 4608}, {3, 0, 0, 558, 764, 568},
        {0, 0, 3, 1118, 1602, 1562}, {4, 0, 5, 8944, 13288, 13064}, {5, 0, 1, 22432, 24886, 22720}, {5, 1, 3, 22432, 26572, 25520},
    };
    for (auto& e : g) {
        PilotConfig pc; pc.fftCode = e.fft; pc.ext = e.ext; pc.pp = e.pp; pc.giIdx = 2;
        PilotMap m(pc);
        CHECK(m.valid(), "invalid %d/%d/%d", e.fft, e.ext, e.pp);
        if (!m.valid()) continue;
        CHECK(m.p2DataCells() == e.cp2, "C_P2 %d vs %d", m.p2DataCells(), e.cp2);
        CHECK(m.dataCells() == e.cdata, "C_DATA %d vs %d (fft %d pp %d)", m.dataCells(), e.cdata, e.fft, e.pp + 1);
        CHECK(m.fcDataCells() == e.nfc, "N_FC %d vs %d", m.fcDataCells(), e.nfc);
    }
    // EN 302 755 table 48: Cdata for every FFT size / pilot pattern / carrier mode, and the same count in every symbol of the pattern.
    // (A mistyped continual pilot, 3636 for 13636 in the 16K extended PP1 list, left one data cell more in one symbol of four: the
    // frequency interleaver of those symbols had the wrong length and a real mux never decoded; the generator shared the table, so
    // the loopback tests could not see it.)
    struct T { int fft, ext, pp, cdata; } t48[] = {
        {3, 0, 0, 764}, {3, 0, 1, 768}, {3, 0, 2, 798}, {3, 0, 3, 804}, {3, 0, 4, 818},
        {0, 0, 0, 1522}, {0, 0, 1, 1532}, {0, 0, 2, 1596}, {0, 0, 3, 1602}, {0, 0, 4, 1632}, {0, 0, 6, 1646},
        {2, 0, 0, 3084}, {2, 0, 1, 3092}, {2, 0, 2, 3228}, {2, 0, 3, 3234}, {2, 0, 4, 3298}, {2, 0, 6, 3328},
        {1, 0, 0, 6208}, {1, 0, 1, 6214}, {1, 0, 2, 6494}, {1, 0, 3, 6498}, {1, 0, 4, 6634}, {1, 0, 6, 6698}, {1, 0, 7, 6698},
        {1, 1, 0, 6296}, {1, 1, 1, 6298}, {1, 1, 2, 6584}, {1, 1, 3, 6588}, {1, 1, 4, 6728}, {1, 1, 6, 6788}, {1, 1, 7, 6788},
        {4, 0, 0, 12418}, {4, 0, 1, 12436}, {4, 0, 2, 12988}, {4, 0, 3, 13002}, {4, 0, 4, 13272}, {4, 0, 5, 13288}, {4, 0, 6, 13416}, {4, 0, 7, 13406},
        {4, 1, 0, 12678}, {4, 1, 1, 12698}, {4, 1, 2, 13262}, {4, 1, 3, 13276}, {4, 1, 4, 13552}, {4, 1, 5, 13568}, {4, 1, 6, 13698}, {4, 1, 7, 13688},
        {5, 0, 1, 24886}, {5, 0, 3, 26022}, {5, 0, 5, 26592}, {5, 0, 6, 26836}, {5, 0, 7, 26812},
        {5, 1, 1, 25412}, {5, 1, 3, 26572}, {5, 1, 5, 27152}, {5, 1, 6, 27404}, {5, 1, 7, 27376},
    };
    for (auto& e : t48) {
        PilotConfig pc; pc.fftCode = e.fft; pc.ext = e.ext; pc.pp = e.pp; pc.giIdx = 2;
        PilotMap m(pc);
        if (!m.valid()) { CHECK(false, "table 48: %d/%d/PP%d invalid", e.fft, e.ext, e.pp + 1); continue; }
        CHECK(m.dataCells() == e.cdata, "table 48: fft %d ext %d PP%d: %d data cells, spec %d", e.fft, e.ext, e.pp + 1, m.dataCells(), e.cdata);
        std::vector<uint8_t> ty;
        const int nP2 = fftModeFromS2(e.fft)->nP2;
        for (int l = nP2; l < nP2 + 2 * m.dy() + 2; l++) {
            m.symbolTypes(l, 200, ty);
            int c = 0;
            for (auto v : ty) c += v == kCellData;
            CHECK(c == e.cdata, "fft %d ext %d PP%d symbol %d: %d data cells, spec %d", e.fft, e.ext, e.pp + 1, l, c, e.cdata);
        }
    }
    PilotConfig bad; bad.fftCode = 5; bad.pp = 0; // 32K does not allow PP1
    CHECK(!PilotMap(bad).valid(), "32K PP1 should be unsupported");
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails;
}
