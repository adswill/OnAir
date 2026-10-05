// ATSC 3.0 L1-Detail: the syntax of A/322 Table 9.8 (pack, unpack, CRC) and the protection chain for FEC modes 1 to 7, including
// segmentation into several coded blocks.
#include "dect2/atsc3_l1.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static L1DetailPlp plp(int id, int layer, int ti) {
    L1DetailPlp p;
    p.id = id; p.layer = layer; p.start = 1000 * id; p.size = 4000 + id; p.scramblerType = 0; p.fecType = 3; p.mod = 2; p.cod = 7; p.tiMode = ti;
    if (ti == 0) p.fecBlockStart = 77;
    if (ti == 1) { p.ctiFecBlockStart = 4000; p.ctiDepth = 2; p.ctiStartRow = 300; }
    if (ti == 2) { p.htiInterSubframe = 1; p.htiNumTiBlocks = 2; p.htiNumFecBlocksMax = 40; p.htiNumFecBlocks = {10, 20, 30}; p.htiCellInterleaver = 1; }
    if (layer == 0) { p.type = 1; p.numSubslices = 5; p.subsliceInterval = 123456; }
    else p.ldmInjectionLevel = 9;
    p.numChannelBonded = 1; p.channelBondingFormat = 2; p.bondedRfId = {1, 5};
    return p;
}

int main() {
    L1Basic b;
    b.timeInfoFlag = 2; b.numSubframes = 1; b.firstSubMimo = 1; b.firstSubSbsFirst = 1;
    L1Detail d;
    d.version = 1; d.bondedBsid = {0x1234}; d.timeSec = 3900000000u; d.timeMsec = 321; d.timeUsec = 654; d.bsid = 0x4321;
    L1DetailSubframe s0, s1;
    s0.subframeMultiplex = 1; s0.frequencyInterleaver = 1; s0.sbsNullCells = 1234;
    s0.plps = {plp(0, 0, 0), plp(1, 1, 0), plp(2, 0, 2)};
    s1.mimo = 0; s1.miso = 1; s1.fftSize = 2; s1.reducedCarriers = 2; s1.guardInterval = 5; s1.numOfdmSymbols = 200;
    s1.scatteredPilotPattern = 6; s1.scatteredPilotBoost = 3; s1.sbsFirst = 0; s1.sbsLast = 1; s1.sbsNullCells = 99;
    s1.frequencyInterleaver = 0;
    s1.plps = {plp(7, 0, 1)};
    d.subframes = {s0, s1};

    int size = l1DetailSizeBytes(b, d);
    auto bits = packL1Detail(b, d, size);
    CHECK((int)bits.size() == size * 8 && size >= 25, "size of the packed structure");
    L1Detail u;
    bool ok = unpackL1Detail(b, bits, u);
    CHECK(ok, "unpack with CRC");
    if (ok) {
        CHECK(u.version == 1 && u.bondedBsid.size() == 1 && u.bondedBsid[0] == 0x1234 && u.timeSec == 3900000000u && u.timeMsec == 321 && u.timeUsec == 654 && u.bsid == 0x4321, "header fields");
        CHECK(u.subframes.size() == 2 && u.subframes[0].plps.size() == 3 && u.subframes[1].plps.size() == 1, "subframes and PLPs");
        const auto& p2 = u.subframes[0].plps[2];
        CHECK(p2.tiMode == 2 && p2.htiNumFecBlocks.size() == 3 && p2.htiNumFecBlocks[2] == 30 && p2.htiCellInterleaver == 1, "hybrid time interleaver fields");
        CHECK(u.subframes[0].plps[1].layer == 1 && u.subframes[0].plps[1].ldmInjectionLevel == 9, "LDM layer");
        CHECK(u.subframes[1].fftSize == 2 && u.subframes[1].guardInterval == 5 && u.subframes[1].numOfdmSymbols == 200 && u.subframes[1].sbsNullCells == 99, "second subframe");
        CHECK(u.subframes[1].plps[0].ctiDepth == 2 && u.subframes[1].plps[0].ctiStartRow == 300 && u.subframes[1].plps[0].bondedRfId.size() == 2, "CTI and channel bonding");
    }
    auto bad = bits; bad[100] ^= 1;
    CHECK(!unpackL1Detail(b, bad, u), "CRC catches a flipped bit");
    // padded to a larger size
    auto big = packL1Detail(b, d, size + 40);
    CHECK((int)big.size() == (size + 40) * 8 && unpackL1Detail(b, big, u), "padded L1D_reserved");
    CHECK(!unpackL1Detail(b, std::vector<uint8_t>(bits.begin(), bits.begin() + 30 * 8), u) || true, "truncated input does not crash");

    // protection of random content for every FEC mode, including multi-block sizes
    std::mt19937 rng(9);
    std::normal_distribution<float> g(0.f, 1.f);
    struct T { int mode; int bytes; double snr; } tests[] = {
        {1, 60, -3}, {1, 180, -3}, {1, 400, -3}, {2, 120, 4}, {2, 500, 4}, {3, 120, 12}, {3, 900, 12}, {4, 150, 18}, {5, 200, 24}, {6, 250, 30}, {7, 300, 34}};
    for (auto& t : tests) {
        std::vector<uint8_t> in(t.bytes * 8);
        for (auto& v : in) v = rng() & 1;
        auto cells = encodeL1Detail(in, t.mode);
        char m[100];
        snprintf(m, sizeof m, "mode %d, %d bytes: cell count %d", t.mode, t.bytes, l1DetailCells(t.bytes, t.mode));
        CHECK(!cells.empty() && (int)cells.size() == l1DetailCells(t.bytes, t.mode), m);
        double pw = 0;
        for (auto& c : cells) pw += std::norm(c);
        pw /= cells.size();
        float sigma = (float)std::sqrt(std::pow(10.0, -t.snr / 10.0) / 2.0);
        for (auto& c : cells) c += cf32(g(rng), g(rng)) * sigma;
        std::vector<uint8_t> out;
        int it = 0;
        bool dec = decodeL1Detail(cells.data(), (int)cells.size(), 2 * sigma * sigma, t.mode, t.bytes, out, &it);
        printf("  mode %d, %3d bytes: %5d cells, SNR %+3.0f dB: %s (%d iterations)\n", t.mode, t.bytes, (int)cells.size(), t.snr, dec && out == in ? "ok" : "FAILED", it);
        snprintf(m, sizeof m, "mode %d, %d bytes decodes", t.mode, t.bytes);
        CHECK(dec && out == in, m);
    }
    // one complete round: pack, protect, noise, decode, unpack
    {
        int mode = 3;
        auto cells = encodeL1Detail(bits, mode);
        float sigma = (float)std::sqrt(std::pow(10.0, -14.0 / 10.0) / 2.0);
        for (auto& c : cells) c += cf32(g(rng), g(rng)) * sigma;
        std::vector<uint8_t> out;
        bool dec = decodeL1Detail(cells.data(), (int)cells.size(), 2 * sigma * sigma, mode, size, out);
        L1Detail v;
        CHECK(dec && unpackL1Detail(b, out, v) && v.bsid == 0x4321, "L1-Detail through the whole chain");
    }
    printf(fails ? "atsc3 l1detail: FAILED\n" : "atsc3 l1detail: ok\n");
    return fails ? 1 : 0;
}
