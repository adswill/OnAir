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
};

struct FrameResult {
    PreambleResult preamble;
    bool ok = false;
    std::vector<PlpResult> plps;
    SubframeParams subframe;
};

FrameResult decodeFrame(const cf32* x, size_t n, const Bootstrap& bs);

// Length of the frame after the bootstrap in samples at the frame's rate: the Preamble and every subframe (from L1; time-aligned excess
// samples are not included). 0 when the parameters are not valid.
size_t frameLengthSamples(const Bootstrap& bs, const L1Basic& l1, const L1Detail& d);

} // namespace atsc3
} // namespace dect2
