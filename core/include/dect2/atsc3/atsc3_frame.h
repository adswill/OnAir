// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ATSC 3.0 physical layer frame, after the bootstrap: the Preamble and one subframe. buildFrame() makes a frame from baseband packets (for
// tests and simulations); decodeFrame() reads the Preamble, finds the PLPs described by L1-Detail, and returns the payload of their
// baseband packets. Time interleaving, LDM, MIMO and further subframes are not part of this version.
#pragma once
#include "atsc3_bicm.h"
#include "atsc3_cellmux.h"
#include "atsc3_preamble.h"
#include <vector>

namespace dect2 {
namespace atsc3 {

struct FramePlp {
    int id = 0;
    int fecType = 0;      // L1D_plp_fec_type: 0 BCH+16K, 1 BCH+64K, 2 CRC+16K, 3 CRC+64K, 4 16K only, 5 64K only
    int mod = 2;          // L1D_plp_mod: 0 QPSK .. 5 4096QAM
    int cod = 6;          // L1D_plp_cod: 0 = 2/15 .. 11 = 13/15
    std::vector<std::vector<uint8_t>> bbPackets;   // transmit side: whole baseband packets (header included), Kpayload / 8 bytes each
};

BicmConfig plpBicm(const FramePlp& p);   // from the L1-Detail fields; nInner = 0 when the combination is not supported

struct FrameSetup {
    Bootstrap bs;                  // preamble_structure selects the Preamble
    int preambleSymbols = 1;
    int preambleReduced = 2;       // L1B_preamble_reduced_carriers (used when there are several Preamble symbols)
    int l1DetailMode = 3;          // FEC mode of L1-Detail, 1..7
    int fftCode = 0, reduced = 0, guardCode = 1, spPattern = 4, spBoost = 1;
    int numSymbols = 40;           // data and boundary symbols of the subframe
    bool sbsFirst = true, sbsLast = true;
    int sbsNullCells = 0;
    bool freqInterleaver = true;
};

// The frame as samples at the frame's sample rate: Preamble, then the subframe. Fails (empty result) when the PLPs do not fit.
std::vector<cf32> buildFrame(const FrameSetup& s, const std::vector<FramePlp>& plps, L1Basic* l1Out = nullptr, int* freeCells = nullptr);

struct PlpResult {
    int id = 0;
    int blocks = 0, blocksOk = 0;
    std::vector<std::vector<uint8_t>> packets;   // the decoded baseband packets (header included), only those that decoded
    std::vector<bool> ok;                         // per block: decoded
    // A PLP with the convolutional time interleaver: its cells wait for the receiver, which de-interleaves in frame order (the interleaver keeps
    // its state from one frame to the next) and cuts the FEC blocks, which may straddle frames.
    bool cti = false;
    std::vector<cf32> cells;
    std::vector<float> cellW;                     // their channel state weights, |h|^2 / mean |h|^2 (empty: none; 4.1)
    float noise = 0.1f;
    int ctiRows = 0, ctiStartRow = 0, ctiFecBlockStart = 0;
    FramePlp cfg;
    // Without time interleaving the FEC blocks of a PLP run on from frame to frame: the frame starts with the end of the last block of the frame
    // before (`head`), then whole blocks (decoded above), then the beginning of a block that ends in the next frame (`tail`). The receiver puts
    // the tail of one frame and the head of the next together.
    bool straddle = false;
    std::vector<cf32> head, tail;
    std::vector<float> headW, tailW;              // their channel state weights (empty: none)
    int skipped = 0;                              // blocks not tried: the PLP is backed off (DecodePlan) and none of its probe blocks decoded
};

// PLPs that have decoded nothing for a while (Atsc3Receiver::decodePlan): of each such PLP only `blocks` FEC blocks, spread over the frame, are
// tried first. If one of them decodes, the whole PLP is decoded as usual; if none does, the other blocks are not tried (they count as failed).
// A PLP that is not listed is decoded in full.
struct DecodePlan {
    struct Probe { int id = 0, blocks = 0; };
    std::vector<Probe> probe;
    int probeBlocks(int id) const { for (auto& p : probe) if (p.id == id) return p.blocks; return 0; }
};

// Decodes FEC blocks of one PLP into `pr` (blocks, ok, packets, blocksOk; descrambled baseband packets), in batches on up to `threads` threads.
// With `probe` > 0 and more blocks than that, only `probe` blocks spread over them are tried first, and the rest only if one of them decoded
// (DecodePlan); the blocks not tried are counted in pr.skipped.
void decodeFecBlocks(const Bicm& bicm, const std::vector<std::vector<cf32>>& blocks, float noise, const std::vector<std::vector<float>>* weights,
                     int probe, int threads, PlpResult& pr);

struct FrameResult {
    PreambleResult preamble;
    bool ok = false;
    std::vector<PlpResult> plps;
    SubframeParams subframe;
    DecodePlan plan;                              // the plan it was decoded with (the receiver applies it to the PLPs it decodes itself)
};

FrameResult decodeFrame(const cf32* x, size_t n, const Bootstrap& bs, const DecodePlan* plan = nullptr);

// Length of the frame after the bootstrap in samples at the frame's rate: the Preamble and every subframe (from L1; time-aligned excess
// samples are not included). 0 when the parameters are not valid.
size_t frameLengthSamples(const Bootstrap& bs, const L1Basic& l1, const L1Detail& d);

} // namespace atsc3
} // namespace dect2
