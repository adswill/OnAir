// ATSC 3.0 LDPC codes (A/322 section 6.1.3 and Annex A), Ninner = 16200. Type A (rates 2/15 to 5/15) has a dual-diagonal part and an
// identity part, with the parity bits of the first part interleaved; Type B (rates 6/15 and up) is the usual accumulator chain.
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 {
namespace atsc3 {

const int* ldpcTable16200(int rate15, const int** lens, int* numRows);   // generated in atsc3_ldpc_tables.cpp
const int* ldpcTable64800(int rate15, const int** lens, int* numRows);   // generated in atsc3_ldpc_tables64.cpp

class Ldpc {
public:
    explicit Ldpc(int rate15, int n = 16200);   // 16200 or 64800 bit code of rate rate15 / 15, 2 <= rate15 <= 13
    int n() const { return n_; }
    int typeBQ() const { return qB_; }   // Q of Type B codes (parity interleaver), 0 for Type A
    bool typeA() const { return typeA_; }
    int k() const { return k_; }
    bool ok() const { return k_ > 0; }

    // bits[0..k) in; bits is extended to n entries (systematic code word, parity bits in the order of A/322 before any parity permutation)
    void encode(std::vector<uint8_t>& bits) const;

    // Normalised min-sum decoding; llr > 0 means bit 0. Returns true when every parity check is satisfied.
    bool decode(const std::vector<float>& llr, int maxIter, std::vector<uint8_t>& hard, int* iters = nullptr) const;

private:
    int rate_, n_ = 16200, k_ = 0, m_ = 0;
    bool typeA_ = false;
    int m1_ = 0, m2_ = 0, q1_ = 0, q2_ = 0, qB_ = 0;
    std::vector<std::vector<int>> rows_;
    std::vector<int> chkStart_, chkVar_;   // check -> variable adjacency
    int accAddr(int x, int m) const;
    void buildGraph();
};

const Ldpc& ldpc16200(int rate15);
const Ldpc& ldpcCode(int n, int rate15);   // cached, 16200 or 64800

} // namespace atsc3
} // namespace dect2
