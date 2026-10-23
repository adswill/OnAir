// RaptorQ (RFC 6330) for the Mercury / hermes-broadcast file carousel: the decoder a receiver needs, and the encoder the test signal
// uses. Written for OnAir from the RFC (sections 4 and 5); the constant tables are the RFC's. One sub-block per source block (N = 1)
// and symbol alignment 1, as Mercury and hermes-broadcast send. Decoding: the precode and LT rows are peeled with inactivation, the
// inactivated columns (and the permanently inactive ones) solved as a dense system over GF(256).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {
namespace mercury {

struct RqParams {
    uint32_t K = 0, Kp = 0, J = 0, S = 0, H = 0, W = 0, L = 0, P = 0, P1 = 0, B = 0;
};
RqParams rqParams(uint32_t K);   // K source symbols of a block (K <= 56403)

// The source block partition of RFC 6330 4.4.1.2: F bytes, symbol size T, Z blocks -> symbols of block sbn
uint32_t rqBlockSymbols(uint32_t F, uint32_t T, uint32_t Z, uint32_t sbn);

// One source block
class RqBlockDecoder {
public:
    RqBlockDecoder(uint32_t K, uint32_t T);
    // an encoding symbol (ESI < K: source, else repair); returns false when it was already there
    bool add(uint32_t esi, const uint8_t* sym);
    uint32_t received() const { return (uint32_t)esis_.size(); }
    uint32_t K() const { return K_; }
    bool done() const { return done_; }
    // try to decode (cheap to call: it only works when there are enough new symbols); true when the block is complete
    bool tryDecode();
    const std::vector<uint8_t>& data() const { return data_; }   // K * T bytes once done
private:
    uint32_t K_, T_;
    std::vector<uint32_t> esis_;
    std::vector<std::vector<uint8_t>> syms_;
    std::vector<uint8_t> data_;
    bool done_ = false;
    uint32_t triedAt_ = 0;
};

// The intermediate symbols of a block from its K source symbols, and any encoding symbol from them
class RqBlockEncoder {
public:
    RqBlockEncoder(uint32_t K, uint32_t T, const uint8_t* source);   // K * T bytes (the last symbol zero padded)
    bool ok() const { return ok_; }
    std::vector<uint8_t> symbol(uint32_t esi) const;
private:
    RqParams p_;
    uint32_t T_;
    std::vector<std::vector<uint8_t>> C_;
    bool ok_ = false;
};

// Solve for the L intermediate symbols from the given rows: ISIs (with their symbols); false when the system is not yet solvable
bool rqSolve(const RqParams& p, uint32_t T, const std::vector<uint32_t>& isi, const std::vector<const uint8_t*>& sym,
             std::vector<std::vector<uint8_t>>& C);
// the encoding symbol of internal symbol index isi from the intermediate symbols
std::vector<uint8_t> rqEnc(const RqParams& p, uint32_t T, const std::vector<std::vector<uint8_t>>& C, uint32_t isi);

} // namespace mercury
} // namespace dect2
