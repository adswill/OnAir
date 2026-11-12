// ATSC 3.0 bit-interleaved coding and modulation for one PLP (A/322 section 6): outer code (BCH, CRC or none), LDPC, parity, group-wise and
// block bit interleavers, and the constellation mapper (QPSK, 16 to 256 point 2D non-uniform, 1024 and 4096 point 1D non-uniform).
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <vector>

namespace dect2 {
namespace atsc3 {

struct BicmConfig {
    int nInner = 64800;      // 16200 or 64800
    int rate15 = 8;          // code rate rate15 / 15, 2 to 13
    int bitsPerCell = 6;     // 2 = QPSK, 4, 6, 8 (NUC), 10, 12 (1D NUC, 64800 only)
    int outer = 0;           // 0 = BCH, 1 = CRC, 2 = none (L1D_plp_fec_type: BCH = 0/1, CRC = 2/3, none = 4/5)
};

class Bicm {
public:
    explicit Bicm(const BicmConfig& c);
    bool ok() const { return ok_; }
    int kPayload() const { return kPayload_; }   // baseband packet bits per FEC frame
    int cells() const { return cfg_.nInner / cfg_.bitsPerCell; }
    const BicmConfig& config() const { return cfg_; }

    std::vector<cf32> encode(const std::vector<uint8_t>& payload) const;
    // `noiseVar`: noise power per cell (both dimensions). Returns true if LDPC converged and the outer code (BCH correction or CRC) agrees.
    bool decode(const cf32* cells, float noiseVar, std::vector<uint8_t>& payload, int* ldpcIterations = nullptr) const;
    // Up to 16 blocks at once with the 8-bit layered decoder (one block per vector lane): much faster per block than decode() when several blocks
    // are waiting. payload[b] and ok[b] as for decode().
    // `weights` (optional, one array of cells() values per block): channel state of each cell, |h|^2 against its mean, so that the soft bits are
    // those of a cell with noise noiseVar / w (4.0, docs/SENSITIVITY-4.0-X3.md); null: every cell with noiseVar
    void decodeBatch(const std::vector<const cf32*>& cells, float noiseVar, std::vector<std::vector<uint8_t>>& payload, std::vector<char>& ok,
                     const std::vector<const float*>* weights = nullptr) const;

    // the constellation, indexed by the label (y0 most significant)
    const std::vector<cf32>& constellation() const { return points_; }
    // position j of the bit-interleaved FEC frame comes from bit perm[j] of the LDPC code word (parity, group-wise and block interleaver combined)
    const std::vector<int>& bitPermutation() const { return perm_; }

private:
    void llrs(const cf32* cells, float noiseVar, std::vector<float>& llr, const float* w = nullptr) const;   // soft bits of the code word, limited
    bool finish(const std::vector<uint8_t>& hard, std::vector<uint8_t>& payload) const;   // outer code on the decoded word
    bool decodeLlr(const std::vector<float>& llr, std::vector<uint8_t>& hard, int* iters = nullptr) const;   // the LDPC code word from float LLRs
    BicmConfig cfg_;
    bool ok_ = false;
    bool quad_ = false;                // the constellation is one quadrant and its reflections: faster soft bits
    std::vector<float> qa_, qb_;       // the points of the first quadrant
    int kPayload_ = 0, kLdpc_ = 0;
    std::vector<cf32> points_;
    std::vector<int> perm_;
};

// Block interleaver of A/322 6.2.3: output index j -> input index (type 0 = A, 1 = B), for Table 6.8 / 6.9 configurations.
std::vector<int> blockInterleaverMap(int nInner, int bitsPerCell, int type);
int blockInterleaverType(int nInner, int bitsPerCell, int rate15);

} // namespace atsc3
} // namespace dect2
