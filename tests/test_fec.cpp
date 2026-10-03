// FEC round trips: BCH error correction and the whole data path (encode -> noise -> demap -> LDPC -> BCH).
// The encoder side was compared bit-for-bit with the open-source gr-dvbt2 transmitter (336 configurations).
#include "dect2/t2fec.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <cstdint>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool chain(const PlpFec& f, float snrDb, int blocks, int tiBlocks, std::mt19937& rng, double* msPerBlock) {
    FecDims d = fecDims(f);
    const LdpcCode& ldpc = ldpcFor(f);
    const BchCode& bch = bchFor(f);
    const auto& map = bitInterleaverMap(f);
    const int bps = d.bitsPerCell, cells = d.cellsPerBlock;
    std::vector<std::vector<uint8_t>> msgs(blocks);
    std::vector<cf32> txCells;
    for (int b = 0; b < blocks; b++) {
        msgs[b].resize(d.kBch);
        for (auto& v : msgs[b]) v = rng() & 1;
        std::vector<uint8_t> bits = msgs[b];
        bch.encode(bits, d.kBch);
        ldpc.encode(bits);
        std::vector<uint16_t> lab(cells);
        for (int c = 0; c < cells; c++) { unsigned l = 0; for (int k = 0; k < bps; k++) l = (l << 1) | bits[map[c * bps + k]]; lab[c] = l; }
        std::vector<cf32> cl;
        qamMapBlock(f, lab, cl);
        txCells.insert(txCells.end(), cl.begin(), cl.end());
    }
    std::vector<cf32> tx;
    cellInterleave(f, blocks, tiBlocks, txCells, tx);
    // channel: AWGN plus a per-cell gain so that noise variance differs per cell (as after equalisation)
    std::normal_distribution<float> nd(0.f, 1.f);
    float n0avg = std::pow(10.f, -snrDb / 10.f) / 2.f;
    std::vector<cf32> rx(tx.size());
    std::vector<float> n0(tx.size());
    for (size_t i = 0; i < tx.size(); i++) {
        float g = 0.6f + 0.8f * ((i * 2654435761u >> 8) % 1000) / 1000.f; // 0.6 .. 1.4
        n0[i] = n0avg / (g * g);
        rx[i] = tx[i] + cf32(nd(rng), nd(rng)) * std::sqrt(n0[i]);
    }
    std::vector<cf32> rd(rx.size());
    std::vector<float> nd0(rx.size());
    cellDeinterleave(f, blocks, tiBlocks, rx.data(), rd.data());
    cellDeinterleaveF(f, blocks, tiBlocks, n0.data(), nd0.data());
    bool ok = true;
    auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; b++) {
        std::vector<float> llrLab((size_t)cells * bps), llr(d.nLdpc);
        qamDemapBlock(f, rd.data() + (size_t)b * cells, nd0.data() + (size_t)b * cells, cells, llrLab.data());
        for (int p = 0; p < d.nLdpc; p++) llr[map[p]] = llrLab[p];
        std::vector<uint8_t> hard;
        ldpc.decodeFast(llr, 50, hard);
        std::vector<uint8_t> bb(hard.begin(), hard.begin() + d.kLdpc);
        int ne = bch.decode(bb);
        bool same = ne >= 0;
        for (int i = 0; same && i < d.kBch; i++) if (bb[i] != msgs[b][i]) same = false;
        if (!same) ok = false;
    }
    *msPerBlock = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / blocks;
    return ok;
}

int main() {
    std::mt19937 rng(3);
    // BCH
    for (int sh = 0; sh < 2; sh++) for (int t : {8, 10, 12}) {
        if (sh == 0 && t == 8) continue;
        PlpFec f; f.shortFrame = sh; f.rate = sh ? 2 : (t == 10 ? 2 : 0);
        FecDims d = fecDims(f);
        if (d.t != t && !(sh && t == 12)) continue;
        const BchCode& b = bchFor(f);
        std::vector<uint8_t> msg(d.kBch);
        for (auto& v : msg) v = rng() & 1;
        std::vector<uint8_t> cw = msg;
        b.encode(cw, d.kBch);
        for (int ne : {0, 1, d.t, d.t + 3}) {
            auto r = cw;
            for (int i = 0; i < ne; i++) r[rng() % r.size()] ^= 1;
            int diff = 0;
            for (size_t i = 0; i < r.size(); i++) diff += r[i] != cw[i];
            int got = b.decode(r);
            if (diff <= d.t) CHECK(got == diff && r == cw, "BCH %s t=%d errors %d decode %d", sh ? "short" : "normal", d.t, diff, got);
            else CHECK(got < 0 || r != cw, "BCH uncorrectable case accepted");
        }
    }
    // data path
    struct C { bool sh; int rate, mod, rot; float snr; int blocks, ti; } cs[] = {
        {false, 2, 2, 1, 21, 6, 3},  // a real-world mux: 64QAM 2/3 rotated, 3 TI blocks
        {false, 0, 0, 0, 5, 4, 0},   {false, 3, 0, 1, 8, 3, 2},   {false, 1, 1, 0, 12, 3, 0},  {false, 5, 1, 1, 17, 3, 3},
        {false, 3, 2, 0, 21, 3, 0},  {false, 4, 3, 1, 28, 3, 3},  {false, 1, 3, 0, 24, 3, 0},
        {true, 0, 0, 0, 5, 4, 0},    {true, 2, 1, 1, 14, 4, 2},    {true, 3, 2, 1, 22, 4, 0},    {true, 6, 0, 0, 3, 3, 0},    {true, 7, 3, 0, 26, 3, 0},
    };
    for (auto& c : cs) {
        PlpFec f; f.shortFrame = c.sh; f.rate = c.rate; f.mod = c.mod; f.rotation = c.rot;
        double ms;
        bool ok = chain(f, c.snr, c.blocks, c.ti, rng, &ms);
        printf("%s %-3s mod %d rot %d @ %4.1f dB TI %d: %s  (%.1f ms/block)\n", c.sh ? "short " : "normal", rateName(c.rate), c.mod, c.rot, c.snr, c.ti, ok ? "ok" : "FAILED", ms);
        CHECK(ok, "data path failed");
    }
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
