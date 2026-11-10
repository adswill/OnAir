// CCSDS pieces shared by the satellite downlinks that are not in meteor_proto.h (which has the K = 7 Viterbi decoder, the
// randomiser and Reed-Solomon (255,223) in the dual basis, interleave 4, and the CADU frame):
// - CRC-16 (CCITT, 0xFFFF start) of the LRIT / HRIT space packets,
// - the CCSDS 121.0-B adaptive Rice (lossless) coder with the unit-delay predictor, as SZIP / libaec use it (GOES HRIT images).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace ccsds {

// CRC-16 CCITT (0x1021, start 0xFFFF, no final XOR)
uint16_t crc16(const uint8_t* p, size_t n, uint16_t crc = 0xFFFF);

struct RiceParams {
    int bitsPerSample = 8;       // 1 .. 16
    int blockSize = 16;          // J: 8, 16, 32 or 64 samples a block
    int rsiBlocks = 128;         // blocks between reference samples (an SZIP scan line)
    bool preprocess = true;      // the unit-delay predictor and mapping (SZIP's NN option)
    bool byteAlignRsi = true;    // every reference sample interval starts on a byte boundary (SZIP); false: the plain CCSDS 121 bit stream (libaec without AEC_PAD_RSI, VIIRS)
};
// SZIP option bits as the HRIT Rice compression header carries them
enum { kSzAllowK13 = 1, kSzChip = 2, kSzEc = 4, kSzLsb = 8, kSzMsb = 16, kSzNn = 32, kSzRaw = 128 };
// Decodes up to `samples` samples. Every reference sample interval starts on a byte boundary (SZIP). Returns the number of samples
// written (fewer than asked when the data ran out or were corrupt).
size_t riceDecode(const uint8_t* in, size_t n, const RiceParams& p, uint16_t* out, size_t samples);
// The encoder (test signals): the last block is padded with the last sample. Picks the cheapest option for every block (zero block,
// second extension, split samples k = 0 .., no compression), so the decoder's paths are all exercised.
std::vector<uint8_t> riceEncode(const uint16_t* in, size_t samples, const RiceParams& p);

} // namespace ccsds
} // namespace dect2
