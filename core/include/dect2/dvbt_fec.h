// DVB-T receive-side channel decoder: equalised data cells in, MPEG transport stream packets out.
#pragma once
#include "dvbt.h"
#include <deque>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace dvbt {

struct FecStats {
    uint64_t packets = 0, rsClean = 0, rsCorrected = 0, rsFailed = 0, bytesCorrected = 0;
    uint64_t symbols = 0;
    bool phaseLocked = false, syncLocked = false;
    int punctPhase = 0;
    double viterbiMargin = 0;     // normalised path metric of the last block (1 = noiseless)
};

class FecDecoder {
public:
    void configure(const Params& p);
    void reset();
    // One OFDM symbol's data carriers in carrier order (1512 / 6048), with the noise variance of each; symIdx 0..67
    void pushSymbol(const cf32* cells, const float* n0, int symIdx);
    // Appends the 188-byte packets decoded so far (transport_error_indicator set on packets RS could not repair)
    void takePackets(std::vector<uint8_t>& out);
    const FecStats& stats() const { return st_; }
    const Params& params() const { return p_; }

private:
    void process(bool flush);
    Params p_;
    FecStats st_;
    std::vector<float> llrQueue_;          // coded-bit LLRs not yet decoded
    std::vector<int8_t> carry_;            // soft values of the last steps (history for the next window)
    std::vector<uint8_t> bits_;            // decoded info bits not yet grouped into bytes
    size_t bitsDropped_ = 0;
    std::vector<uint8_t> bytes_;           // byte stream (interleaved domain) waiting for sync / interleaver
    int bitOffset_ = -1;                   // bit phase of the byte boundary, -1 until found
    std::vector<uint8_t> after_;           // after the outer de-interleaver, waiting for 204-byte blocks
    int syncBytePos_ = -1;
    ConvInterleaver deint_{true};
    int groupIdx_ = 0;
    bool haveGroup_ = false;
    int warm_ = 0;                         // blocks still to discard while the de-interleaver fills
    std::vector<uint8_t> out_;
    int blocksSinceSync_ = 0, syncMisses_ = 0;
    float llrScale_ = 8.f;
    unsigned prbs_ = 0xA9;
    void descramblePacket(uint8_t* pkt, int g);
};

} // namespace dvbt
} // namespace dect2
