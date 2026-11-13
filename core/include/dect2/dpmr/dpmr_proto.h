// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// dPMR mode 1 air interface (ETSI TS 102 490, peer to peer, 6.25 kHz): the parts both the receiver and the test signal use.
//
// Four-level FSK, 2400 symbols/s (4800 bit/s), outer deviation +-1050 Hz; dibits 01 +3, 00 +1, 10 -1, 11 -3.
// Frame syncs: FS1 (header, 24 symbols), FS2 (payload frames, 12), FS3 (end frame, 24), FS4 (packet data header, 24).
//
// A transmission: preamble, header frame, payload superframes, end frame.
//   header   FS1 or FS4 | HI0 (120 bits) | colour code (24 bits) | HI1 (120 bits, HI0 repeated)            156 symbols
//   payload  FS2 (frames 0, 2) or colour code (frames 1, 3) | CCH (72 bits) | TCH (288 bits)              192 symbols, 80 ms
//   end      FS3 | END (72 bits)                                                                          60 symbols
// HI: 80 bits (72 + CRC-8) in ten Hamming (12,8) words; CCH: 48 bits (41 + CRC-7) in six; END: 48 bits (41 + CRC-7) in six.
// Every block is written as rows of Hamming words and read out by columns (block interleaving), then XORed with PN9
// (x^9 + x^5 + 1, all ones at the start of each block). TCH: four 72-bit AMBE+2 voice frames, or a 24-byte data block in 24 Hamming words.
// The colour code (0 to 63) is sent as 12 symbols, two per bit (1: +3 -3, 0: -3 +3).
// The superframe CCH carries the called id (frames 0, 1) and own id (frames 2, 3) in 12-bit halves, so a late entry finds both.
#pragma once
#include <cstdint>
#include <string>

namespace dect2 {
namespace dpmr {

constexpr int kFrameSym = 192, kHdrSym = 156, kEndSym = 60, kFsLong = 24, kFsShort = 12, kCcSym = 12;
constexpr int kHiBits = 120, kCchBits = 72, kEndBits = 72, kTchBits = 288;
extern const int kFs1[kFsLong], kFs3[kFsLong], kFs4[kFsLong], kFs2[kFsShort];
inline int dibitToSym(int d) { return d == 1 ? 3 : d == 0 ? 1 : d == 2 ? -1 : -3; }
void ccSymbols(int cc, int out[kCcSym]);
int ccDecode(const float* v, float* score = nullptr);   // v: 12 normalised symbols; the best colour code

uint32_t crc8(const uint8_t* bits, int n);
uint32_t crc7(const uint8_t* bits, int n);
uint32_t crc16(const uint8_t* bits, int n);
void hamEncode(const uint8_t d[8], uint8_t c[12]);
int hamDecode(uint8_t c[12]);                          // corrects one error in place; returns 0 clean, 1 corrected, -1 more
void pn9(uint8_t* out, int n);

// a block of nWords Hamming words (8 * nWords info bits) to 12 * nWords air bits (interleaved, scrambled) and back
void blockEncode(const uint8_t* info, int nWords, uint8_t* air);
int blockDecode(const uint8_t* air, int nWords, uint8_t* info);   // returns bits corrected, -1 when a word could not be
// the same from soft values in air order (> 0 a 1, the magnitude the confidence; before the descrambling): maximum likelihood per word
int blockDecodeSoft(const float* soft, int nWords, uint8_t* info);

enum HdrType { kHdrComm = 0, kHdrConnReq = 1, kHdrUnconnect = 2, kHdrAck = 3, kHdrSysReq = 4, kHdrStatus = 5 };
enum CommMode { kVoice = 0, kVoiceSlow = 1, kData1 = 2, kData2 = 3, kPacket = 4, kVoiceData = 5 };
enum CallType { kIndividual = 0, kGroup = 1, kAllCall = 2 };

struct Header {
    int type = kHdrComm;
    uint32_t called = 0, own = 0;    // 24-bit ids
    int commMode = kVoice, callType = kGroup;
    bool emergency = false;
    int extra = 0;                   // status message: the status code; data: the block count
};
void headerBits(const Header& h, uint8_t info[80]);
bool headerParse(const uint8_t info[80], Header& h);

struct Cch {
    int frame = 0;                   // 0 to 3 in the superframe
    int idPart = 0;                  // 12 bits: called high, called low, own high, own low
    int commMode = 0, version = 0, callType = kGroup;
    bool emergency = false;
    uint32_t slowData = 0;           // 18 bits
};
void cchBits(const Cch& c, uint8_t info[48]);
bool cchParse(const uint8_t info[48], Cch& c);

struct End { int type = 0; uint32_t own = 0; };
void endBits(const End& e, uint8_t info[48]);
bool endParse(const uint8_t info[48], End& e);

// data block: block number, 21 characters, CRC-16; 192 bits
void dataBits(int block, const std::string& text21, uint8_t info[192]);
bool dataParse(const uint8_t info[192], int& block, std::string& text);

const char* hdrTypeName(int t);
const char* commModeName(int m);
const char* callTypeName(int t);
double pmr446Mhz(int ch);            // PMR446 digital channel 1 to 16: 446.103125 + (ch - 1) * 0.00625
std::string idText(uint32_t id);     // dPMR ids are shown as 8-digit numbers (the "all" id 0xFFFFFF as ALL)

} // namespace dpmr
} // namespace dect2
