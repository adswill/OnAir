// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRAPOL physical frames, written from PAS 0001-2 v3.0.0 (TETRAPOL Forum, public): the 160-bit / 20 ms frame of the GMSK BT 0.25,
// 8 kbit/s channel (7.1, 7.2), voice frames (6.1) and data frames (6.2). Every table and constant below is from that text (section cited
// at each). Receive only: nothing here decrypts anything.
//
// One frame on air (transmission order):
//   f0..f7 = 0 1 1 0 0 0 1 0 (6.1.5.2 / 6.2.5.2; 0x46 sent LSB first), then f8+k = e'k + S(SCR, k) for the 152 coded bits; the modulator
//   codes the whole stream differentially, Mk = mk + mk-1 (7.2), and the downlink is spectrally inverted (7.3).
// The 152 coded bits (6.1.1 .. 6.1.5, 6.2.1 .. 6.2.5):
//   protected part:  26 bits -> 52 bits, cyclic rate-1/2 code C2j = b"j + b"j-1 + b"j-2, C2j+1 = b"j + b"j-2 (6.1.2 / 6.2.2), b"-2 = b"24,
//                    b"-1 = b"25
//   second part:     data frame: 48 bits + 2 zero tail bits -> 100 bits, the same code started from zero (6.2.1, 6.2.2: b"-2 = b"-1 =
//                    b"48 = b"49 = 0, which is the cyclic code on those 50 bits); voice frame: 100 bits, not coded
//   interleaving e_K(j) = c_j (VHF formula 6.1.3.1 / 6.2.3.1, UHF tables 6.1.4.1 / 6.2.4.1), UHF only: differential precoding with
//   PRE_COD (6.1.4.2 / 6.2.4.2), then the 127-bit scrambling sequence s(k) = s(k-1) + s(k-7), s(0..6) = 1, from offset SCR (6.1.5.1).
// Information bits:
//   voice frame: d=0, v0..v19, ASB X, ASB Y, CRC-3 (I + D + D^3, the three bits complemented) | v20..v119 not coded   (6.1, 6.1.1)
//   data frame:  d=1, FN0, FN1, 64 data bits, ASB X, ASB Y, CRC-5 (I + D^2 + D^5), 2 zero bits                        (6.2, 6.2.1)
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
