// ATSC 3.0 L1-Basic: packing with CRC, the protection chain for modes 1 to 5 (cell counts of A/322 Table 6.17, decoding in noise), and the
// complete path through a Preamble OFDM symbol.
#include "dect2/atsc3_l1.h"
#include "dect2/atsc3_ofdm.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static L1Basic sample() {
    L1Basic l;
    l.version = 0; l.llsFlag = 1; l.timeInfoFlag = 2; l.paprReduction = 1; l.frameLengthMode = 0; l.frameLength = 200;
    l.excessSamplesPerSymbol = 1234; l.numSubframes = 1; l.preambleNumSymbols = 2; l.preambleReducedCarriers = 3;
    l.l1DetailContentTag = 2; l.l1DetailSizeBytes = 417; l.l1DetailFecType = 2; l.l1DetailAdditionalParityMode = 1;
    l.l1DetailTotalCells = 70000; l.firstSubFftSize = 2; l.firstSubReducedCarriers = 1; l.firstSubGuardInterval = 6;
    l.firstSubNumOfdmSymbols = 311; l.firstSubScatteredPilotPattern = 9; l.firstSubScatteredPilotBoost = 2; l.firstSubSbsLast = 1;
    return l;
}

static bool same(const L1Basic& a, const L1Basic& b) {
    return a.version == b.version && a.llsFlag == b.llsFlag && a.timeInfoFlag == b.timeInfoFlag && a.paprReduction == b.paprReduction &&
           a.frameLengthMode == b.frameLengthMode && a.frameLength == b.frameLength && a.excessSamplesPerSymbol == b.excessSamplesPerSymbol &&
           a.numSubframes == b.numSubframes && a.preambleNumSymbols == b.preambleNumSymbols && a.preambleReducedCarriers == b.preambleReducedCarriers &&
           a.l1DetailContentTag == b.l1DetailContentTag && a.l1DetailSizeBytes == b.l1DetailSizeBytes && a.l1DetailFecType == b.l1DetailFecType &&
           a.l1DetailAdditionalParityMode == b.l1DetailAdditionalParityMode && a.l1DetailTotalCells == b.l1DetailTotalCells &&
           a.firstSubFftSize == b.firstSubFftSize && a.firstSubReducedCarriers == b.firstSubReducedCarriers &&
           a.firstSubGuardInterval == b.firstSubGuardInterval && a.firstSubNumOfdmSymbols == b.firstSubNumOfdmSymbols &&
           a.firstSubScatteredPilotPattern == b.firstSubScatteredPilotPattern && a.firstSubScatteredPilotBoost == b.firstSubScatteredPilotBoost &&
           a.firstSubSbsFirst == b.firstSubSbsFirst && a.firstSubSbsLast == b.firstSubSbsLast;
}

int main() {
    L1Basic l = sample();
    auto bits = packL1Basic(l);
    CHECK((int)bits.size() == 200, "L1-Basic is 200 bits");
    L1Basic u;
    CHECK(unpackL1Basic(bits, u) && same(l, u), "pack and unpack");
    bits[40] ^= 1;
    CHECK(!unpackL1Basic(bits, u), "CRC catches a flipped bit");
    L1Basic sym = sample();
    sym.frameLengthMode = 1; sym.timeOffset = 4321; sym.additionalSamples = 0;
    CHECK(unpackL1Basic(packL1Basic(sym), u) && u.timeOffset == 4321 && u.frameLengthMode == 1, "symbol-aligned variant");

    // cells per mode: A/322 Table 6.17
    const int want[5] = {3820, 934, 484, 259, 163};
    for (int m = 1; m <= 5; m++) {
        auto cells = encodeL1Basic(l, m);
        char msg[80];
        snprintf(msg, sizeof msg, "mode %d produces %d cells", m, want[m - 1]);
        CHECK((int)cells.size() == want[m - 1] && l1BasicCells(m) == want[m - 1], msg);
        double pw = 0;
        for (auto& c : cells) pw += std::norm(c);
        pw /= cells.size();
        snprintf(msg, sizeof msg, "unit power constellation, mode %d (%.3f)", m, pw);
        CHECK(std::fabs(pw - 1.0) < 0.15, msg);   // random data on few cells: the average fluctuates
    }

    // decoding in noise: every mode, at a signal-to-noise ratio around its operating point and well below it
    std::mt19937 rng(5);
    std::normal_distribution<float> g(0.f, 1.f);
    struct T { int mode; double snrDb; bool expectOk; } tests[] = {
        {1, -3, true}, {1, -6, true}, {2, 0, true}, {3, 6, true}, {4, 12, true}, {5, 15, true}, {1, -14, false}, {5, -2, false}};
    for (auto& t : tests) {
        int okc = 0, trials = 6;
        for (int k = 0; k < trials; k++) {
            L1Basic v = sample();
            v.l1DetailTotalCells = 1000 * (k + 1);
            auto cells = encodeL1Basic(v, t.mode);
            float sigma = (float)std::sqrt(std::pow(10.0, -t.snrDb / 10.0) / 2.0);
            for (auto& c : cells) c += cf32(g(rng), g(rng)) * sigma;
            L1Basic o;
            bool ok = decodeL1Basic(cells.data(), (int)cells.size(), 2 * sigma * sigma, t.mode, o);
            okc += ok && same(v, o);
        }
        printf("  mode %d at %+.0f dB: %d/%d decoded\n", t.mode, t.snrDb, okc, trials);
        char msg[80];
        snprintf(msg, sizeof msg, "mode %d at %+.0f dB", t.mode, t.snrDb);
        if (t.expectOk) CHECK(okc == trials, msg); else CHECK(okc == 0, msg);
    }

    // the whole path: L1-Basic in the first Preamble symbol, through the channel
    PreambleParams pp;
    CHECK(preambleParams(0, pp), "preamble_structure 0");
    int cred = 4;
    int nData = (int)preambleDataCarriers(pp, cred).size();
    auto cells = encodeL1Basic(l, pp.l1BasicMode);
    std::vector<cf32> sym0(nData, cf32(0.7071f, 0.7071f));
    for (size_t i = 0; i < cells.size(); i++) sym0[i] = cells[i];
    auto tx = modulatePreambleSymbol(pp, cred, 0, sym0);
    std::vector<cf32> rx(tx);
    for (size_t i = 0; i + 30 < tx.size(); i++) rx[i + 30] += tx[i] * cf32(0.2f, -0.15f);
    float sigma = (float)std::sqrt(std::pow(10.0, -2.0 / 10.0) / 2.0);
    for (auto& v : rx) v += cf32(g(rng), g(rng)) * sigma * 0.5f;
    std::vector<cf32> eq;
    float nv = 0;
    CHECK(demodulatePreambleSymbol(pp, cred, 0, rx.data(), eq, &nv), "preamble demodulation");
    L1Basic got;
    bool ok = decodeL1Basic(eq.data(), (int)eq.size(), std::max(nv, 0.05f), pp.l1BasicMode, got);
    CHECK(ok && same(l, got), "L1-Basic through a Preamble symbol with echo and noise");

    printf(fails ? "atsc3 l1: FAILED\n" : "atsc3 l1: ok\n");
    return fails ? 1 : 0;
}
