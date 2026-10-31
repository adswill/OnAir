// Inmarsat Aero P channel, the coding layer between the demodulated bits and the signal-unit bytes:
// unique word, frame header, interleaver, rate 1/2 convolutional code, scrambler, byte packing.
// Every constant follows JAERO (github.com/jontio/JAERO, JAERO/aerol.cpp and aerol.h, jconvolutionalcodec.cpp), which is the
// reference decoder for these channels; the clause or line it comes from is named next to it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {

// aerol.cpp: preambledetector.setPreamble(3780831379LL,32); setPreamble pushes bit len-1 first, so the most significant bit is sent first
constexpr uint32_t kAeroUw = 0xE15AE893u;
constexpr int kAeroUwBits = 32;

// C channel (circuit-mode voice, 8400 bit/s, aerol.cpp DecodeC and setSettings case 8400): the unique word is 52 bits on each OQPSK arm,
// one arm carries 0xAB376938BCA30 and the other 0xC53D1C96ECD5 (aerol.cpp preambledetectorreal/imag.setPreamble(216866263330005LL,
// 3012071630031408LL, 52), most significant bit first); each arm's detector takes either word, straight or inverted, so the I/Q ambiguity
// of the carrier loop does not matter. The frame after the word is 4096 channel bits: 16 interleaver blocks of 64 x 4, a rate 3/4 punctured
// code (every fourth coded bit of the rate 1/2 code is left out: depunture_soft_block(.., 4, ..) puts an erasure after every three received
// bits), decoded to 2730 bits of which the first 2714 are the frame, scrambled from the frame start. Those 2714 bits are 24 sub-frames of
// 1 + 96 + 12 bits (a spare bit, one 96-bit coded voice frame, 12 signalling bits) and a 25th voice frame at bit 2617; the 24 x 12
// signalling bits, packed low bit first, are three 12-byte signal units with the usual check (Call progress, Telephony acknowledge, fill-in).
constexpr uint64_t kAeroCUwQ = 216866263330005ull;    // 0xC53D1C96ECD5 (JAERO's comment: the Q arm)
constexpr uint64_t kAeroCUwI = 3012071630031408ull;   // 0xAB376938BCA30 (the I arm)
constexpr int kAeroCUwBits = 52;                      // per arm; 104 channel bits
constexpr int kAeroCFrameBits = 4096;                 // channel bits after the word
constexpr int kAeroCCodedPairs = 2730;                // after the erasures are put back: 5460 soft values
constexpr int kAeroCInfoBits = 2714;                  // decoded bits that are the frame
constexpr int kAeroCSubFrames = 24;
constexpr int kAeroCSubFrameBits = 109;               // 1 + 96 + 12
constexpr int kAeroCVoiceBits = 96;                   // one coded voice frame (12 bytes)
constexpr int kAeroCVoiceFrames = 25;                 // 24 in the sub-frames and one more at bit 2617
constexpr int kAeroCVoiceBytes = kAeroCVoiceFrames * 12;
constexpr int kAeroCSuBytes = 36;                     // 24 x 12 signalling bits = 3 SUs

// The frame of one P channel (aerol.cpp setSettings):
//   600 / 1200 bit/s: UW 32 + header 16 + 1152 coded bits = 1200 bits (2 s / 1 s); 3 or 2 interleaver blocks of 64 x 6 / 64 x 9
//   10500 bit/s:      UW on both arms 64 + header 16 + 178 unused + 4992 coded bits = 5250 bits (0.5 s); one block of 64 x 78
//   8400 bit/s (C):   UW 52 on each arm = 104 + 4096 coded bits = 4200 bits (0.5 s); 16 blocks of 64 x 4, punctured (see above)
struct AeroFrameFormat {
    int bitRate = 0;
    int uwBits = 0;          // channel bits taken by the unique word (32, or 64 when it is sent on both OQPSK arms, 104 on the C channel)
    int headerBits = 16;
    int skipBits = 0;        // bits after the header that are not data (10500: 178)
    int codedBits = 0;       // 1152, 4992 or 4096
    int cols = 0;            // interleaver columns N (rows are always 64)
    int blocks = 0;          // interleaver blocks per frame
    int totalBits() const { return uwBits + headerBits + skipBits + codedBits; }
    int blockBits() const { return 64 * cols; }
    int infoBits() const { return cChannel() ? kAeroCInfoBits : codedBits / 2; }
    int infoBytes() const { return cChannel() ? kAeroCSuBytes : codedBits / 16; }   // 72 or 312 bytes = 6 or 26 SUs of 12 bytes; C: the 3 SUs
    bool oqpsk() const { return bitRate == 10500 || bitRate == 8400; }
    bool cChannel() const { return bitRate == 8400; }
    double rrcRollOff() const { return bitRate == 8400 ? 0.6 : 1.0; }   // oqpskdemodulator.cpp: rrc.design(0.6, .., fb/2) for 8400, else 1.0
};
const AeroFrameFormat* aeroFrameFormat(int bitRate);   // nullptr for a rate that is not 600, 1200, 8400 or 10500

// Header (aerol.cpp Decode): 16 bits, first bit = most significant; format id (must be 1), superframe marker, two frame counters
inline uint16_t aeroHeader(int formatId, int superframe, int count1, int count2) {
    return (uint16_t)(((formatId & 15) << 12) | ((superframe & 15) << 8) | ((count1 & 15) << 4) | (count2 & 15));
}

// Scrambler (aerol.h AeroLScrambler): additive, 15-bit register, starts again at every frame
class AeroScrambler {
public:
    AeroScrambler();
    void reset() { pos_ = 0; }
    int next() { const int v = seq_[pos_]; pos_ = (pos_ + 1) % (int)seq_.size(); return v; }
    static const std::vector<uint8_t>& sequence();     // the first 5000 bits from reset
private:
    const std::vector<uint8_t>& seq_;
    int pos_ = 0;
};

// Interleaver (aerol.cpp AeroLInterleaver): 64 rows, row permutation (i*27) % 64
// interleave: tx[i*N + j] = coded[permute[i] + 64*j], permute[(i*27)%64] = i
void aeroInterleave(const uint8_t* coded, uint8_t* tx, int cols);
// deinterleave: out[j*64 + i] = rx[((i*27)%64) * N + j]
void aeroDeinterleave(const float* rx, float* out, int cols);

// Convolutional code: rate 1/2, K = 7, polynomials 109 and 79 (aerol.cpp SetCode(2,7,{109,79},..) handed to libcorrect, which shifts the newest
// bit into the least significant end of the register; the first coded bit of each pair uses 109)
constexpr int kAeroPoly0 = 109, kAeroPoly1 = 79;
class AeroConvEncoder {
public:
    void reset() { sr_ = 0; }
    void encode(int bit, uint8_t& c0, uint8_t& c1);
private:
    unsigned sr_ = 0;
};

// Soft Viterbi decoder that carries its path metrics from one call to the next (like JAERO's Decode_Continuous);
// a call is decided at its own end from the best state. The frame decoder hands it a whole frame at once.
// Soft input: positive = 1, magnitude = confidence.
class AeroViterbi {
public:
    AeroViterbi();
    void reset();                                                     // forget the past: every state equally likely
    void decode(const float* soft, int nPairs, uint8_t* bits);        // 2*nPairs soft values -> nPairs bits
private:
    std::vector<float> pm_, pmNew_;
    std::vector<uint64_t> dec_;       // one 64-bit word of survivor decisions per step
    uint8_t out0_[128], out1_[128];
};

// SU check (aerol.h AeroLcrc16): CRC-16 reflected 0x8408, start 0xFFFF, inverted at the end (CRC-16/X-25),
// over bytes 0..9 of each 12-byte SU, sent low byte first in bytes 10 and 11
uint16_t aeroCrc16(const uint8_t* p, size_t n);
bool aeroSuCrcOk(const uint8_t* su12);
void aeroSuSetCrc(uint8_t* su12);

// Bytes <-> bits (aerol.cpp: the first decoded bit lands in bit 0 of the byte)
void aeroBytesToBits(const uint8_t* bytes, size_t nBytes, uint8_t* bits);
void aeroBitsToBytes(const uint8_t* bits, size_t nBytes, uint8_t* bytes);

// Transmit side: the channel bits of one frame (UW, header, data), from the frame's information bytes (infoBytes() of them).
// The encoder runs on from frame to frame; the scrambler starts again in every frame.
class AeroFrameEncoder {
public:
    explicit AeroFrameEncoder(int bitRate);
    std::vector<uint8_t> frame(const uint8_t* info, uint16_t header);
    // C channel: the 36 bytes of the three SUs and the 25 coded voice frames (kAeroCVoiceBytes); header is not sent on this channel
    std::vector<uint8_t> cFrame(const uint8_t* su36, const uint8_t* voice300);
    const AeroFrameFormat& format() const { return fmt_; }
private:
    AeroFrameFormat fmt_;
    AeroConvEncoder enc_;
    uint32_t fill_ = 0x1234567u;
};

// C channel: the 2714 frame bits <-> the three SUs and the 25 voice frames (aerol.cpp DecodeC, the sub-frame loops)
void aeroCUnpack(const uint8_t* bits2714, uint8_t* su36, uint8_t* voice300);
void aeroCPack(const uint8_t* su36, const uint8_t* voice300, uint8_t* bits2714);

// Receive side, after the unique word: soft data bits of one frame -> information bytes.
struct AeroDecodedFrame {
    int bitRate = 0;
    uint16_t header = 0;
    std::vector<uint8_t> bytes;        // infoBytes() bytes (C channel: the 36 bytes of its three SUs)
    std::vector<uint8_t> voice;        // C channel only: 25 coded voice frames of 12 bytes, in frame order
    int susOk = 0, susBad = 0;         // 12-byte SUs whose CRC holds / fails
    float channelBer = 0;              // decided bits re-encoded and compared with the hard channel bits: the error rate before decoding
};
class AeroFrameDecoder {
public:
    explicit AeroFrameDecoder(int bitRate);
    void reset() { vit_.reset(); }
    // coded: codedBits soft values in channel order (as received, interleaved)
    void decode(const float* coded, uint16_t header, AeroDecodedFrame& out);
    const AeroFrameFormat& format() const { return fmt_; }
private:
    void decodeC(const float* coded, AeroDecodedFrame& out);
    AeroFrameFormat fmt_;
    AeroViterbi vit_;
    AeroConvEncoder reenc_;
    std::vector<float> deint_, dep_;
    std::vector<uint8_t> bits_;
};

} // namespace dect2
