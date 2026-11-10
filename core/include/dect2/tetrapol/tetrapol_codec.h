// TETRAPOL physical frames (PAS 0001-2 clause 6): the 160-bit / 20 ms frame of the GMSK 8 kbit/s channel, voice (6.1) and data (6.2).
// Written from the published air-interface description; the open tetrapol-kit project (GPL-2, not included, see THIRD_PARTY.md) was read
// as a cross-check of the clause 6 tables. Receive only: nothing here decrypts anything.
//
// One frame on air (transmission order):
//   8 header bits (0x46 sent LSB first: 0 1 1 0 0 0 1 0) + 152 data bits, then the whole stream is differentially encoded e[k] = x[k] ^ x[k-1].
// The 152 data bits of a frame, from the information bits (6.1.2 / 6.2.2 ... 6.1.5 / 6.2.5):
//   protected part:  26 bits -> 52 bits, circular rate-1/2 code  c[2i] = u[i]^u[i-1]^u[i-2], c[2i+1] = u[i]^u[i-2]  (indices mod 26)
//   second part:     data frame: 50 bits -> 100 bits, the same circular code on its own 50 bits;  voice frame: 100 bits, not coded
//   interleave (out[T[j]] = in[j]; data / voice tables, VHF / UHF), UHF only: differential precoding, then the 127-bit scrambling
//   sequence (x^7 + x^6 + 1 form, s[k] = s[k-1] ^ s[k-7], seven ones first) from offset SCR (0 = not scrambled).
// Information bits:
//   voice frame: d=0, voice1[20], asb[2], crc3[3]  | voice2[100]              (CRC-3 x^3+x+1 over the first 23 bits, sent inverted)
//   data frame:  d=1, data[66], asb[2], crc5[5], zero[2]                     (CRC-5 x^5+x^2+1 over the first 69 bits)
// The 120 speech-codec bits of a voice frame are voice1[0..19] then voice2[0..99] (PAS 0001-7 numbering 0..119).
#pragma once
#include <cstdint>

namespace dect2 {
namespace tetrapol {

constexpr int kFrameBits = 160;
constexpr int kHdrBits = 8;
constexpr int kDataBits = 152;
constexpr int kPayloadBits = 66;   // data[66] of a data frame: data[0..1] the block frame number, data[2..65] 64 bits of the data block
constexpr int kVoiceBits = 120;    // speech-codec bits of a voice frame
constexpr int kScrMax = 127;
constexpr uint8_t kFrameHdr = 0x46;   // PAS 0001-2 6.1.5.2 / 6.2.5.2

enum Band { kVhf = 1, kUhf = 2 };
enum FrameType { kVoice = 0, kData = 1 };

struct DataFrame {
    uint8_t payload[kPayloadBits] = {};   // data[0..65], 0 or 1
    uint8_t asb[2] = {};
    int scramble = 0;                     // 0 .. 127 (0: not scrambled)
    int metric = 0;                       // coded bits that disagree with the decoded frame (0: none)
    bool ok = false;
};

struct VoiceFrame {
    uint8_t codec[kVoiceBits] = {};       // voice1[0..19] then voice2[0..99]
    uint8_t asb[2] = {};
    int scramble = 0;
    int metric = 0;                       // errors in the protected part (52 coded bits)
    bool ok = false;                      // CRC-3 of the protected part matched
};

// One decoded frame of either type. type is the d bit; ok means the CRC (and for data the zero bits) matched.
struct Frame {
    int type = -1;
    DataFrame data;
    VoiceFrame voice;
    bool ok = false;
    int metric = 0;
};

// Encoders (test signals): bits160 are the frame before the transmission differential (header then the 152 data bits).
void encodeDataFrame(const DataFrame& in, int band, uint8_t bits160[kFrameBits]);
void encodeVoiceFrame(const VoiceFrame& in, int band, uint8_t bits160[kFrameBits]);

// Decoder of the 152 data bits of one frame, after the transmission differential has been undone (frameBits152 below).
// forceType: -1 the d bit decides, kVoice / kData only that type.
// rel152 (optional): the reliability (|soft value|) of each of the 152 received bits before the transmission differential is undone; the
// circular code is then decoded with soft decisions (about 2 dB better than hard ones). metric stays the count of hard bit disagreements.
bool decodeFrame(const uint8_t bits152[kDataBits], int band, int scr, Frame& out, int forceType = -1, const float* rel152 = nullptr);

// The transmission differential over a stream (e[k] = x[k] ^ prev, prev = x[k]); returns the last x (the carry into the next frame).
uint8_t diffEncode(uint8_t* bits, int n, uint8_t prev);
// Raw received bits of one frame (160, starting at the header) to the 152 data bits: e[1..7] are the header, x[7] = 0 by the header.
// invert: the received stream has the other polarity (PAS 0001-2: the uplink is the inverse of the downlink).
void frameBits152(const uint8_t raw160[kFrameBits], bool invert, uint8_t out152[kDataBits]);
// Errors of the 7 header bits after the differential (raw e[1..7] against 1 0 1 0 0 1 1, or its inverse).
int syncErrors(const uint8_t* raw, bool invert);

// CRCs and tables, for the tests
void crc5(const uint8_t* bits, int n, uint8_t out[5]);
void crc3(const uint8_t* bits, int n, uint8_t out[3]);
uint8_t scrambleBit(int k, int scr);
int interleaveIndex(int j, int band, int type);

} // namespace tetrapol
} // namespace dect2
