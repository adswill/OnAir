// Inmarsat Aero C channel (circuit-mode voice, ground station to aircraft): the layer between the 8400 bit/s OQPSK soft bits and the
// coded voice frames. Every constant and every step follows JAERO (github.com/jontio/JAERO, MIT, Jonti Olds): AeroL::DecodeC and
// AeroL::setSettings in aerol.cpp, AeroLScrambler in aerol.h, PuncturedCode::depunture_soft_block, the 8400 settings of
// oqpskdemodulator.cpp (root raised cosine, roll-off 0.6). The signal-unit types come from the same file (AEROTypeC).
//
// One frame lasts 500 ms:
//   unique word  52 symbols on each arm = 104 channel bits (the two 52-bit words below; either word may be on either arm, either polarity)
//   data         4096 channel bits = 16 blocks of 256, each deinterleaved as 64 rows x 4 columns (the P channel interleaver, aeroDeinterleave)
//                -> depunctured (an erasure after every third bit: the rate 1/2 code is punctured to 2/3) = 5460 values
//                -> Viterbi (K = 7, polynomials 109 and 79, the P channel code) = 2730 bits, the first 2714 are used
//                -> scrambler (the P channel scrambler, restarted at the frame start)
//   the 2714 bits: 24 sub-blocks of 109 bits (1 lead bit, 96 voice bits, 12 signalling bits), then a last lead bit + 96 voice bits + 1 bit
//                = 25 voice frames of 12 bytes (AMBE, 4.8 kbit/s with its error protection: the user-installed codec "ambe") and
//                24 x 12 signalling bits = 36 bytes = 3 signal units of 12 bytes (type, AES id, GES id, ..., CRC-16 as in the P channel)
// Bytes are packed least significant bit first, as for the P channel (aeroBitsToBytes).
// Signal units seen here: 0x01 fill-in, 0x30 call progress (octets 1..3 AES id, octet 4 GES id), 0x60 telephony acknowledge.
#pragma once
#include "aero_phy.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dect2 {

constexpr int kAeroCBitRate = 8400;
constexpr int kAeroCUwSyms = 52;
constexpr int kAeroCUwBits = 2 * kAeroCUwSyms;
constexpr uint64_t kAeroCUwQ = 216866263330005ULL;     // aerol.cpp setPreamble(216866263330005LL, 3012071630031408LL, 52); the most significant bit goes first
constexpr uint64_t kAeroCUwI = 3012071630031408ULL;
constexpr int kAeroCDataBits = 4096;
constexpr int kAeroCFrameBits = kAeroCUwBits + kAeroCDataBits;    // 4200 bits = 500 ms
constexpr int kAeroCCodedPairs = 2730;                 // trellis steps behind the 5460 depunctured values
constexpr int kAeroCInfoBits = 2714;                   // bits used after decoding
constexpr int kAeroCSubBits = 109;                     // lead bit + 96 voice bits + 12 signalling bits
constexpr int kAeroCVoiceFrames = 25;
constexpr int kAeroCVoiceBytes = 12;
constexpr int kAeroCSus = 3;

enum AeroCSuType : uint8_t { kAeroCFillIn = 0x01, kAeroCCallProgress = 0x30, kAeroCTelAck = 0x60 };

// ---- coding (transmit side is for the test signal generator) ----
// The 4200 channel bits of one frame from 25 x 12 voice bytes and 3 x 12 signal-unit bytes (their CRC is the caller's: aeroSuSetCrc).
// The convolutional encoder state is kept from frame to frame; `lead` is the value of the 25 lead bits (not known to carry anything).
class AeroCFrameEncoder {
public:
    std::vector<uint8_t> frame(const uint8_t* voice300, const uint8_t* su36, uint8_t lead = 0);
private:
    AeroConvEncoder enc_;
};

struct AeroCDecoded {
    std::vector<uint8_t> voice;          // 25 x 12 bytes
    std::vector<uint8_t> su;             // 3 x 12 bytes
    bool suOk[kAeroCSus] = {false, false, false};
    float channelBer = 0;                // re-encoded decisions against the hard channel bits (erasures left out)
};
class AeroCFrameDecoder {
public:
    AeroCFrameDecoder();
    // coded: the 4096 soft values after the unique word (positive = 1), polarity already corrected
    void decode(const float* coded, AeroCDecoded& out);
private:
    AeroViterbi vit_;
    AeroConvEncoder reenc_;
    std::vector<float> deint_, dep_;
    std::vector<uint8_t> bits_;
};

// ---- framing ----
struct AeroCFrameEvent {
    std::vector<uint8_t> voice;          // 25 x 12 bytes, in time order, each 12 bytes one frame for the codec
    std::vector<uint8_t> su;             // 3 x 12 bytes
    bool suOk[kAeroCSus] = {false, false, false};
    int uwErrors = 0;                    // bit errors in the unique word (both arms); -1 when the frame was taken on timing alone
    float channelBer = 0;
    float ebn0Db = 0;
    double freqHz = 0;
    uint64_t bitIndex = 0;
    // the AES (aircraft) and GES (ground station) ids of a call-progress SU in this frame, 0 / false when none
    bool haveCall = false;
    uint32_t aesId = 0;
    uint8_t gesId = 0;
    bool voiceLooksUsed = true;          // false when every voice byte is the same (an idle channel)
};

// soft bits (the demodulator's, even positions on the Q arm) -> frames: unique word search on both arms, flywheel, decoding
class AeroCFramer {
public:
    AeroCFramer();
    ~AeroCFramer();
    void reset();
    void push(float soft);               // one channel bit, positive = 1
    void setCallback(std::function<void(AeroCFrameEvent&)> cb);
    bool synced() const;
    bool dataLock() const;               // the SU checks or the channel error rate have been good lately
    uint64_t frames() const;
    uint64_t uwMisses() const;
    uint64_t falseSyncs() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The unique word as channel bits (even positions Q, odd I), for the transmit side and the tests
std::vector<uint8_t> aeroCUniqueWordBits();

} // namespace dect2
