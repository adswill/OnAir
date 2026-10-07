// DTMB forward error correction: the LDPC codes, BCH(762,752), the scrambler, the 4QAM-NR inner code and the convolutional interleaver.
#pragma once
#include "dtmb_defs.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace dect2::dtmb {

// ---------------------------------------------------------------- LDPC
// Quasi-cyclic codes with 127 x 127 circulants. A block (row, col, shift): check i of the block row contains variable (i + shift) mod 127 of the block column.
struct LdpcBlock { uint8_t row, col, shift; };
const LdpcBlock* ldpcBlocks(Rate r, int& count, int& blockRows);

class LdpcCode {
public:
    explicit LdpcCode(Rate r);
    Rate rate() const { return rate_; }
    int blockRows() const { return c_; }              // 35, 23, 11
    int parityBits() const { return c_ * kLdpcZ; }
    int infoBits() const { return e_ * kLdpcZ; }      // 3048, 4572, 6096
    // Systematic encoding: `info` holds infoBits() bits (one per byte); `sent` receives the 7488 transmitted bits: the parity bits from the
    // sixth on (the first five are punctured), then the information bits.
    void encode(const uint8_t* info, uint8_t* sent) const;
    // The same with the whole 7493 bit word (all parity bits, then the information), for tests
    void encodeFull(const uint8_t* info, uint8_t* word) const;
    // Number of unsatisfied checks of a full 7493 bit word (one bit per byte)
    int syndromeWeight(const uint8_t* word) const;
    // Edges of the parity check matrix: (block row, variable) pairs, for tests and statistics
    int edgeCount() const { return (int)blocks_.size() * kLdpcZ; }

    struct Result { bool ok = false; int iterations = 0; };
    // Layered normalised min-sum decoder; one per thread.
    class Decoder {
    public:
        explicit Decoder(const LdpcCode& code);
        // llr[0 .. 7488): log-likelihood ratios of the transmitted bits, positive favours zero. info receives infoBits() bits.
        // The decoder stops at the first iteration whose parity checks all hold.
        // A word that is plainly hopeless (many erased LLRs, or still over a third of the checks unsatisfied after 12 iterations) is given up early.
        Result decode(const float* llr, uint8_t* info, int maxIter, float alpha = 0.6f);
    private:
        const LdpcCode& code_;
        std::vector<float> lv_, r_, q_;
        std::vector<uint64_t> hard_;
    };

private:
    friend class Decoder;
    struct Blk { uint64_t lo, hi; };
    struct Edge { int col, shift; };
    Rate rate_;
    int c_ = 0, e_ = 0;
    std::vector<LdpcBlock> blocks_;
    std::vector<std::vector<Edge>> rows_;       // per block row, all edges
    std::vector<int> edgeBase_;                 // first edge number of every block row
    // encoder plan
    std::vector<int> gap_;
    std::vector<std::pair<int, int>> order_;    // (row used as pivot, unknown block solved)
    std::vector<int> left_;                     // rows that become the gap equations
    std::vector<std::vector<uint64_t>> ainv_;   // inverse of the gap matrix, rows of 6 words
    void planEncoder();
    void run(const std::vector<Blk>& s, const std::vector<Blk>& g, std::vector<Blk>& p, std::vector<Blk>& resid) const;
};

// Shared code objects (built on first use, read-only afterwards)
const LdpcCode& ldpcCode(Rate r);

// ---------------------------------------------------------------- BCH(762,752)
// Systematic: 752 message bits then 10 parity bits, generator x^10 + x^3 + 1 (single error correcting). One bit per byte.
void bchEncode(const uint8_t* msg752, uint8_t* word762);
// 0: clean, 1: one bit corrected in place, -1: not correctable
int bchDecode(uint8_t* word762);

// ---------------------------------------------------------------- scrambler
// PRBS of 1 + x^14 + x^15, initial state 100101010000000, restarted at the start of every signal frame's payload
class Scrambler {
public:
    Scrambler() { reset(); }
    void reset() { s_ = 0xA9; }
    uint8_t next() {
        const uint32_t b = ((s_ >> 13) ^ (s_ >> 14)) & 1u;
        s_ = ((s_ << 1) & 0x7FFFu) | b;
        return (uint8_t)b;
    }
private:
    uint32_t s_;
};

// ---------------------------------------------------------------- 4QAM-NR
// Soft decoding of one block: 16 bit LLRs (x0 .. x7, y0 .. y7; positive favours zero) -> 8 LLRs of x0 .. x7 (max-log)
void nrSoftDecode(const float* llr16, float* llr8);

// ---------------------------------------------------------------- convolutional interleaver
// 52 branches, branch b holds b * M elements (the deinterleaver (51 - b) * M); the input is switched over the branches in turn.
template <class T>
class ConvInterleaver {
public:
    ConvInterleaver(int m, bool inverse) : m_(m) {
        base_.resize(kBranches); len_.resize(kBranches); pos_.assign(kBranches, 0);
        size_t total = 0;
        for (int b = 0; b < kBranches; b++) {
            len_[(size_t)b] = (size_t)(inverse ? (kBranches - 1 - b) : b) * (size_t)m;
            base_[(size_t)b] = total;
            total += len_[(size_t)b];
        }
        buf_.assign(total, T());
    }
    void reset(const T& fill = T()) {
        std::fill(buf_.begin(), buf_.end(), fill);
        std::fill(pos_.begin(), pos_.end(), (size_t)0);
        branch_ = 0;
    }
    // Number of elements an element is delayed by after interleaving and deinterleaving: 51 * 52 * M
    long totalDelay() const { return (long)(kBranches - 1) * kBranches * m_; }
    void process(const T* in, T* out, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const size_t b = branch_;
            const size_t len = len_[b];
            if (len == 0) out[i] = in[i];
            else {
                T& cell = buf_[base_[b] + pos_[b]];
                const T v = in[i];
                out[i] = cell;
                cell = v;
                if (++pos_[b] == len) pos_[b] = 0;
            }
            if (++branch_ == (size_t)kBranches) branch_ = 0;
        }
    }
private:
    int m_;
    std::vector<size_t> base_, len_, pos_;
    std::vector<T> buf_;
    size_t branch_ = 0;
};

} // namespace dect2::dtmb
