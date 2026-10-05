// ATSC 3.0 time interleavers: the cell interleaver shift values printed in A/322, the Convolutional Time Interleaver (stream in, same
// stream out after the latency, start-up with the PRBS initial state), and the twisted block interleaver (a permutation that undoes itself).
#include "dect2/atsc3_bicm.h"
#include "dect2/atsc3_ti.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static bool same(const cf32& a, const cf32& b) { return std::abs(a - b) < 1e-6f; }

int main() {
    CHECK(ctiRows(0, false) == 512 && ctiRows(1, false) == 724 && ctiRows(2, false) == 887 && ctiRows(3, false) == 1024 && ctiRows(2, true) == 1254 && ctiRows(3, true) == 1448, "CTI depths");

    // cell interleaver, the example of 7.1.5.2: 10800 cells per FEC block, shift values 0, 8192, 4096, 2048, 10240, 6144, 1024, 9216
    {
        const int n = 10800;
        auto p0 = htiCellPermutation(n, 0);
        auto base = p0;
        const int shifts[8] = {0, 8192, 4096, 2048, 10240, 6144, 1024, 9216};
        bool ok = true;
        for (int r = 0; r < 8; r++) {
            auto p = htiCellPermutation(n, r);
            // L_r(q) = (L_0(q) + P(r)) mod n
            for (int q = 0; q < n && ok; q++) ok = p[q] == (base[q] + shifts[r]) % n;
        }
        CHECK(ok, "cell interleaver shift values of the example");
        std::vector<char> seen(n, 0);
        bool perm = true;
        for (int v : base) { if (v < 0 || v >= n || seen[v]) perm = false; else seen[v] = 1; }
        CHECK(perm, "cell interleaver base sequence is a permutation");
    }

    std::mt19937 rng(41);
    // CTI with a small number of rows
    {
        BicmConfig c; c.nInner = 16200; c.rate15 = 8; c.bitsPerCell = 6;
        Bicm b(c);
        const int rows = 37, startRow = 5;
        CtiInterleaver tx(rows, 6, b.constellation(), startRow);
        CtiDeinterleaver rx(rows, startRow);
        std::vector<cf32> in(5000);
        for (auto& v : in) v = cf32((float)(rng() % 1000) / 1000.f, (float)(rng() % 1000) / 1000.f);
        auto mid = tx.process(in);
        auto out = rx.process(mid);
        const int lat = rx.latency();
        bool ok = true;
        for (size_t i = lat; i < in.size(); i++) ok &= same(out[i], in[i - lat]);
        CHECK(ok, "CTI round trip after the latency");
        // the first cells after start-up carry the initial state, which are valid constellation points
        int inCons = 0;
        for (int i = 0; i < 200; i++) {
            bool found = false;
            for (auto& p : b.constellation()) found |= same(p, mid[i]);
            inCons += found;
        }
        // cells of row 0 are the input itself, the others come from the initial state: roughly (rows-1)/rows of the first cells
        CHECK(inCons > 150, "CTI start-up cells come from the constellation");
        CHECK(!same(mid[10], in[10]) || !same(mid[11], in[11]), "CTI changes the order");
    }
    // CTI with one of the real depths
    {
        BicmConfig c; c.nInner = 16200; c.rate15 = 6; c.bitsPerCell = 2;
        Bicm b(c);
        const int rows = ctiRows(0, false);
        CtiInterleaver tx(rows, 2, b.constellation(), 0);
        CtiDeinterleaver rx(rows, 0);
        std::vector<cf32> in(rows * (rows - 1) + 20000);
        for (auto& v : in) v = cf32((float)(rng() % 1000), (float)(rng() % 1000));
        auto out = rx.process(tx.process(in));
        bool ok = true;
        for (size_t i = rx.latency(); i < in.size(); i++) ok &= same(out[i], in[i - rx.latency()]);
        CHECK(ok, "CTI round trip with 512 rows");
        printf("  CTI 512 rows: latency %d cells\n", rx.latency());
    }

    // twisted block interleaver and the whole HTI chain
    struct H { int cells, nFec, nMax; bool ci; } hs[] = {{2025, 4, 4, true}, {2700, 3, 5, true}, {8100, 6, 8, false}, {2025, 1, 1, true}, {5400, 2, 6, true}};
    for (auto& h : hs) {
        auto order = htiTwistedReadOrder(h.cells, h.nFec, h.nMax);
        char m[100];
        snprintf(m, sizeof m, "twisted read order has all %d cells (%d x %d of %d)", h.cells * h.nFec, h.cells, h.nFec, h.nMax);
        CHECK((int)order.size() == h.cells * h.nFec, m);
        std::vector<char> seen(order.size(), 0);
        bool perm = true;
        for (int v : order) { if (v < 0 || v >= (int)order.size() || seen[v]) perm = false; else seen[v] = 1; }
        CHECK(perm, "twisted read order is a permutation");
        std::vector<cf32> in(h.cells * h.nFec);
        for (size_t i = 0; i < in.size(); i++) in[i] = cf32((float)i, (float)(i % 17));
        auto x = htiInterleave(in, h.cells, h.nFec, h.nMax, h.ci);
        auto y = htiDeinterleave(x, h.cells, h.nFec, h.nMax, h.ci);
        bool ok = y.size() == in.size();
        for (size_t i = 0; ok && i < in.size(); i++) ok &= same(y[i], in[i]);
        snprintf(m, sizeof m, "HTI round trip (%d cells, %d of %d blocks, cell interleaver %d)", h.cells, h.nFec, h.nMax, h.ci);
        CHECK(ok, m);
        if (h.nFec > 1 || h.ci) CHECK(x != in, "HTI changes the order");
    }
    CHECK(htiBlocksInTiBlock(10, 3, 0) == 3 && htiBlocksInTiBlock(10, 3, 1) == 3 && htiBlocksInTiBlock(10, 3, 2) == 4 && htiBlocksInTiBlock(7, 1, 0) == 7, "FEC blocks per TI block");
    printf(fails ? "atsc3 ti: FAILED\n" : "atsc3 ti: ok\n");
    return fails ? 1 : 0;
}
