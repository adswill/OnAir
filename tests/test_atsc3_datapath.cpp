// ATSC 3.0 data path without time interleaving: baseband packets are scrambled, protected (BICM), multiplexed into the data cells of a
// subframe (one non-dispersed and one dispersed PLP), sent through a channel with an echo and noise, and recovered.
#include "dect2/atsc3_bb.h"
#include "dect2/atsc3_bicm.h"
#include "dect2/atsc3_cellmux.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main() {
    // allocation: indices of a dispersed PLP
    PlpAlloc d; d.start = 100; d.size = 10; d.dispersed = true; d.numSubslices = 3; d.subsliceInterval = 50;
    // subslice size ceil(10 / 3) = 4: cells 0..3 at 100.., 4..7 at 150.., 8..9 at 200..
    CHECK(plpCellIndex(d, 0) == 100 && plpCellIndex(d, 3) == 103 && plpCellIndex(d, 4) == 150 && plpCellIndex(d, 7) == 153 && plpCellIndex(d, 8) == 200 && plpCellIndex(d, 9) == 201, "subslice indices");

    std::mt19937 rng(31);
    std::normal_distribution<float> g(0.f, 1.f);
    SubframeParams s;
    s.fftSize = 8192; s.guard = 512; s.spPattern = 4; s.spBoost = 2; s.cred = 0;   // SP6_2
    s.freqInterleaver = true; s.sbsFirst = true; s.sbsLast = true; s.sbsNullCells = 10; s.numSymbols = 40; s.fiOffset = 1;

    BicmConfig c1; c1.nInner = 16200; c1.rate15 = 8; c1.bitsPerCell = 6; c1.outer = 0;    // 64QAM-NUC 8/15
    BicmConfig c2; c2.nInner = 16200; c2.rate15 = 5; c2.bitsPerCell = 2; c2.outer = 1;    // QPSK 5/15, CRC
    Bicm b1(c1), b2(c2);
    CHECK(b1.ok() && b2.ok(), "BICM configurations");
    const int blocks1 = 12, blocks2 = 6;
    std::vector<std::vector<uint8_t>> payload1, payload2;
    std::vector<cf32> cells1, cells2;
    auto make = [&](Bicm& b, int count, std::vector<std::vector<uint8_t>>& store, std::vector<cf32>& cells, int counterBase) {
        for (int i = 0; i < count; i++) {
            std::vector<uint8_t> alp(b.kPayload() / 8);
            for (auto& v : alp) v = rng() & 0xFF;
            auto pk = makeBbPacket(b.kPayload() / 8, alp, 0, (counterBase + i) & 0xFFFF);
            auto bits = bytesToBits(pk);
            store.push_back(bits);
            bbScramble(bits);
            auto c = b.encode(bits);
            cells.insert(cells.end(), c.begin(), c.end());
        }
    };
    make(b1, blocks1, payload1, cells1, 0);
    make(b2, blocks2, payload2, cells2, 100);
    PlpAlloc a1; a1.start = 0; a1.size = (int)cells1.size();
    PlpAlloc a2; a2.start = a1.size + 500; a2.size = (int)cells2.size(); a2.dispersed = true; a2.numSubslices = 4; a2.subsliceInterval = (a2.size / 4 + 77);
    long total = subframeTotalCells(s, 0);
    printf("  subframe: %ld active cells, PLP 1: %d cells (%d blocks), PLP 2: %d cells (%d blocks)\n", total, a1.size, blocks1, a2.size, blocks2);
    CHECK(plpCellIndex(a2, a2.size - 1) < total && a1.size + 500 + 100 < total, "PLPs fit into the subframe");

    auto active = multiplexSubframe(s, 0, {{a1, cells1}, {a2, cells2}});
    CHECK((long)active.size() == total, "multiplexed cell count");
    auto tx = modulateSubframe(s, active);
    std::vector<cf32> rx(tx.size() + 100, cf32(0, 0));
    for (size_t i = 0; i < tx.size(); i++) rx[i] += tx[i];
    for (size_t i = 0; i + 30 < tx.size(); i++) rx[i + 30] += tx[i] * cf32(0.2f, -0.12f);   // an echo within the guard interval
    double sig = 0;
    for (size_t i = 0; i < tx.size(); i++) sig += std::norm(rx[i]);
    sig /= tx.size();
    float sigma = (float)std::sqrt(sig / std::pow(10.0, 27.0 / 10.0) / 2.0);
    for (auto& v : rx) v += cf32(g(rng), g(rng)) * sigma;

    std::vector<cf32> got;
    std::vector<float> nvs;
    CHECK(demodulateSubframe(s, rx.data(), rx.size(), got, &nvs) && got.size() == active.size(), "subframe demodulation");
    double nvm = 0;
    for (float v : nvs) nvm += v;
    nvm /= std::max<size_t>(1, nvs.size());

    auto decodePlp = [&](Bicm& b, const PlpAlloc& a, const std::vector<std::vector<uint8_t>>& want, int blocks, int counterBase) {
        auto pc = extractPlpCells(got, a);
        int ok = 0;
        for (int i = 0; i < blocks; i++) {
            std::vector<uint8_t> bits;
            int it = 0;
            if (!b.decode(pc.data() + (size_t)i * b.cells(), (float)std::max(nvm, 0.001), bits, &it)) continue;
            bbScramble(bits);
            BbHeader h;
            auto bytes = bitsToBytes(bits);
            if (bits == want[i] && parseBbHeader(bytes.data(), (int)bytes.size(), h) && h.counter == ((counterBase + i) & 0xFFFF)) ok++;
        }
        return ok;
    };
    int ok1 = decodePlp(b1, a1, payload1, blocks1, 0), ok2 = decodePlp(b2, a2, payload2, blocks2, 100);
    printf("  PLP 1: %d/%d baseband packets recovered, PLP 2: %d/%d (noise estimate %.4f)\n", ok1, blocks1, ok2, blocks2, nvm);
    CHECK(ok1 == blocks1, "PLP 1 (non-dispersed) recovered");
    CHECK(ok2 == blocks2, "PLP 2 (dispersed) recovered");
    printf(fails ? "atsc3 datapath: FAILED\n" : "atsc3 datapath: ok\n");
    return fails ? 1 : 0;
}
