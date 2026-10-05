// ATSC 3.0 Preamble (A/322 7.2.5): L1-Basic in the first Preamble symbol, L1-Detail interleaved over all Preamble symbols.
// Builds the symbols from the L1 content (for tests) and decodes them (the receiver side).
#pragma once
#include "atsc3_bootstrap.h"
#include "atsc3_l1.h"
#include "atsc3_ofdm.h"
#include <vector>

namespace dect2 {
namespace atsc3 {

// The time-domain Preamble symbols (guard interval + useful part each, at the sample rate of the frame, back to back) for the given
// bootstrap contents and L1. `fillSeed` seeds the filler cells that carry no signalling (payload cells in the real signal).
// l1.l1DetailSizeBytes, l1DetailFecType and l1DetailTotalCells are set here to match the L1-Detail that is passed in.
// `leftover` receives the number of cells at the end of the last Preamble symbol that carry no signaling (they belong to the first subframe's
// data cell list); `payload`, if given, is put into them (the rest is filler).
std::vector<cf32> buildPreamble(const Bootstrap& bs, L1Basic& l1, const L1Detail& detail, unsigned fillSeed = 1, int* leftover = nullptr, const std::vector<cf32>* payload = nullptr);
// The number of leftover cells for this L1 (needs the L1-Basic of the frame and the preamble parameters).
int preambleLeftoverCells(const Bootstrap& bs, const L1Basic& l1);

struct PreambleResult {
    bool basicOk = false, detailOk = false;
    L1Basic basic;
    L1Detail detail;
    float noiseVar = 0;          // noise power per cell measured from the pilots of the first symbol
    int ldpcIterationsBasic = 0;
    int numSymbols = 0;
    size_t samples = 0;          // length of the whole Preamble in samples
    std::vector<cf32> leftover;  // the cells after L1-Detail in the last Preamble symbol (first cells of the first subframe)
    float leftoverNoise = 0;
};

// `x` points at the first sample of the first Preamble symbol (at the frame's sample rate, after the bootstrap).
PreambleResult decodePreamble(const cf32* x, size_t n, const Bootstrap& bs);

} // namespace atsc3
} // namespace dect2
