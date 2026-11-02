// AO-40 FEC framing, as FUNcube-1 (AO-73), Nayif-1 and others send it on BPSK 1200 baud: 256 bytes -> two interleaved Reed-Solomon (160,128)
// codewords -> CCSDS scrambler -> rate 1/2, K = 7 convolutional code (polynomials 79 and -109 in the GNU Radio notation: 0x4F, and 0x6D with its
// output inverted), terminated -> 5132 symbols, spread over a 80 x 65 matrix whose first 65 positions carry a distributed sync vector (one
// sync bit every 80 symbols) -> 5200 symbols. Follows the AO-40 FEC description (Phil Karn KA9Q, James Miller G3RUH) as implemented in
// gr-satellites' ao40_fec_deframer (Daniel Estevez EA4GPZ, GPL-3, qa vectors); verified against that project's real recording test vector.
// Soft symbols: positive = bit 1.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace dect2 {
namespace cubesat {

constexpr int kAo40SyncBits = 65, kAo40Step = 80, kAo40Window = 5200, kAo40Coded = 5132, kAo40Frame = 256;

// One window of 5200 soft symbols starting at the first sync bit: the 256 byte frame, the Reed-Solomon byte errors put right, and (optionally)
// the Viterbi output bytes. False when a Reed-Solomon codeword cannot be decoded.
bool ao40DecodeWindow(const float* soft5200, std::vector<uint8_t>& frame, int& rsErrors, std::vector<uint8_t>* postViterbi = nullptr);
// The 5200 symbols (+1 / -1) of a 256 byte frame
std::vector<float> ao40Encode(const std::vector<uint8_t>& frame);
// Soft-decision Viterbi for the code above: n symbols (even) -> n / 2 bits, the path ending in state 0. Returns false when no path.
bool ao40Viterbi(const float* soft, size_t n, std::vector<uint8_t>& bits);

// Finds the distributed sync vector in a symbol stream (either polarity, up to maxSyncErrors wrong sync bits) and decodes each frame.
class Ao40Hunter {
public:
    std::function<void(const std::vector<uint8_t>& frame, int rsErrors)> onFrame;
    int maxSyncErrors = 8;
    void reset();
    void push(float soft);                          // one symbol, positive = bit 1
    uint64_t syncs = 0, failed = 0, frames = 0;     // sync matches, of which the code could not decode, frames decoded
private:
    std::vector<float> buf_;                        // the last kAo40Window symbols (ring, position pos_)
    size_t pos_ = 0, filled_ = 0;
    size_t skip_ = 0;                               // symbols to wait after a decoded frame
};

} // namespace cubesat
} // namespace dect2
