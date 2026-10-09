// CDR (GY/T 268.1-2013) LDPC codes of the service data: code words of 9216 bits, rates 1/4, 1/3, 1/2 and 3/4 (5.2.2, Annex D).
// The code word is the information bits followed by the parity bits: c = (m_0 .. m_k-1, p_0 .. p_9215-k), H c^T = 0.
#pragma once
#include <cstdint>
#include <vector>

namespace dect2::cdr {

constexpr int kLdpcZ = 256;            // circulant size
constexpr int kLdpcBits = 9216;        // code word length (36 circulants)

// Rate index as signalled in the system information (b32 b33): 0 1/4, 1 1/3, 2 1/2, 3 3/4
int ldpcInfoBits(int rate);            // 2304, 3072, 4608, 6912
const char* ldpcRateText(int rate);    // "1/4" ...

// The published matrices: blocks (row, col, shift) mean that check i of block row `row` contains variable col * 256 + (i + shift) mod 256.
struct LdpcBlock { uint8_t row, col, shift; };
// Rows first .. first + count - 1 of block (row, col, shift) leave that entry out (as printed in Annex D).
struct LdpcHole { uint8_t row, col, shift; uint16_t first, count; };
const LdpcBlock* ldpcBlocks(int rate, int& count);
const LdpcHole* ldpcHoles(int rate, int& count);

class CdrLdpc {
public:
    explicit CdrLdpc(int rate);
    int rate() const { return rate_; }
    int k() const { return k_; }
    int checks() const { return m_; }
    // The variables of check row r (ascending), for tests against the rows quoted in the standard
    std::vector<int> row(int r) const;
    bool encoderReady() const { return encOk_; }   // false if the parity part could not be inverted (never for the published codes)
    // info: k bits (one per byte); word receives 9216 bits (information, then parity)
    void encode(const uint8_t* info, uint8_t* word) const;
    int syndromeWeight(const uint8_t* word) const;

    struct Result { bool ok = false; int iterations = 0; };
    // Layered normalised min-sum. llr[9216]: positive favours 0. info receives the k information bits (the hard decision even when it fails).
    Result decode(const float* llr, uint8_t* info, int maxIter = 40) const;

private:
    int rate_, k_, m_;
    std::vector<int> rowStart_, rowVar_;         // check -> variables (CSR)
    std::vector<int> colStart_, colChk_;         // information variable -> checks (for the encoder)
    std::vector<uint64_t> gen_;                  // parity of every information bit: k rows of (m / 64) words
    int genWords_ = 0;
    bool encOk_ = false;
    void buildEncoder();
};

// Shared, built on first use (thread safe), read-only afterwards
const CdrLdpc& cdrLdpc(int rate);

} // namespace dect2::cdr
