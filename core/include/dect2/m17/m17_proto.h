// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// M17 protocol layers (M17 specification 2.0, spec.m17project.org / github.com/M17-Project/M17_spec): the 16-bit CRC, base-40
// callsigns, the extended Golay(24,12) code of the LICH, the K=5 rate 1/2 convolutional code with the P1 / P2 / P3 puncturing, the QPP
// interleaver, the randomizer, the frame builders (LSF, stream, packet) and their decoders from soft symbols, and the LSF META contents
// (text, GNSS position, extended callsigns). Nothing here decrypts: an encrypted stream is only reported as such.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace m17 {

constexpr int kSymRate = 4800;
constexpr int kFrameSyms = 192;      // 8 sync + 184 payload symbols, 40 ms
constexpr int kPayloadBits = 368;
constexpr uint16_t kSyncLsf = 0x55F7, kSyncStream = 0xFF5D, kSyncPacket = 0x75FF, kSyncBert = 0xDF55, kSyncEot = 0x555D;
constexpr uint64_t kBroadcast = 0xFFFFFFFFFFFFull;

uint16_t crc16(const uint8_t* d, size_t n);                 // poly 0x5935, init 0xFFFF, not reflected

uint64_t encodeCallsign(const std::string& cs);             // up to 9 characters of " A-Z0-9-/."; "@ALL" -> broadcast
std::string decodeCallsign(uint64_t v);                     // "@ALL", "" for 0, "#<hex>" outside the base-40 range

uint32_t golayEncode(uint16_t data12);                      // data in bits 23..12, check bits 11..1, parity bit 0
bool golayDecode(uint32_t cw, uint16_t& data12, int& errs);  // up to 3 errors corrected; false when more are seen

// The Link Setup Frame: DST, SRC, TYPE, META (and the CRC when packed)
struct Lsf {
    uint64_t dst = 0, src = 0;
    uint16_t type = 0;
    uint8_t meta[14] = {};
};
void lsfPack(const Lsf& l, uint8_t out[30]);                // with the CRC
bool lsfUnpack(const uint8_t in[30], Lsf& l);               // false when the CRC fails

// TYPE field (stream: bit 0 packet/stream, 1-2 data type, 3-4 encryption type, 5-6 subtype, 7-10 CAN, 11 signed)
inline bool typeStream(uint16_t t) { return t & 1; }
inline int typeData(uint16_t t) { return (t >> 1) & 3; }     // 1 data, 2 voice (Codec 2 3200), 3 voice + data (Codec 2 1600)
inline int typeEnc(uint16_t t) { return (t >> 3) & 3; }      // 0 none, 1 scrambler, 2 AES, 3 other
inline int typeEncSub(uint16_t t) { return (t >> 5) & 3; }   // without encryption: the META contents, 0 text, 1 GNSS, 2 extended callsign
inline int typeCan(uint16_t t) { return (t >> 7) & 15; }
inline bool typeSigned(uint16_t t) { return (t >> 11) & 1; }
uint16_t makeType(bool stream, int data, int enc, int sub, int can);
std::string typeText(uint16_t t);                           // "voice 3200", "voice + data 1600", "data", "packet"
std::string encText(uint16_t t);                            // "" when clear, "AES-256", "scrambler 16-bit" ...

// META contents (encryption type none)
struct Gnss {
    int source = 0, station = 0;        // 0 M17 client, 1 OpenRTX, 15 other; 0 fixed, 1 mobile, 2 handheld, 15 other
    bool posValid = false, altValid = false, velValid = false;
    int radius = 0;                     // 3-bit HDOP class
    int bearing = 0;                    // degrees
    double lat = 0, lon = 0;            // degrees
    double altM = 0;
    double speedKmh = 0;
};
void gnssPack(const Gnss& g, uint8_t meta[14]);
Gnss gnssUnpack(const uint8_t meta[14]);
// Text: control byte (high nibble: blocks of the message, low nibble: this block), 13 bytes of text
void textBlockPack(const std::string& msg, int block, uint8_t meta[14]);   // block 0..3 of the message (padded with spaces)
int textBlockCount(const std::string& msg);                 // 1..4
const char* stationText(int st);

// Frame builders: the 368 type-4 bits (interleaved, not yet randomized), one bit per byte
void buildLsf(const uint8_t lsf30[30], uint8_t bits[368]);
void buildStream(const uint8_t lsf30[30], int lichCnt, uint16_t fn, const uint8_t payload[16], uint8_t bits[368]);
void buildPacket(const uint8_t chunk[25], bool eof, int counter, uint8_t bits[368]);
// The 192 symbols (+-1, +-3) of a frame: the sync burst, then the randomized payload
void frameSymbols(uint16_t sync, const uint8_t bits[368], int8_t sym[192]);
void syncSymbols(uint16_t sync, int8_t sym[8]);

// Root-raised-cosine taps (alpha 0.5) at sps samples per symbol over span symbols, scaled so that the filter twice (transmit and
// receive) gives 1 at the symbol instant
std::vector<float> rrcTaps(int sps, int span);

// Receive side. soft[]: 184 payload symbols scaled to +-1 / +-3, turned into 368 soft bits (positive = 1), derandomized and deinterleaved
void softBits(const float sym[184], float soft[368]);
// Viterbi over a depunctured stream; returns the bits that disagreed with the hard decisions of the received (non-erased) bits
int viterbi(const float* soft2, int nIn, uint8_t* out);     // soft2: 2 * nIn values (erasures 0), out: nIn bits (the 4 flush bits included)
bool decodeLsf(const float soft[368], uint8_t lsf30[30], int& corrected);
bool decodeStream(const float soft[368], uint8_t lich6[6], int& golayErrs, uint16_t& fn, uint8_t payload[16], int& corrected);
void decodePacket(const float soft[368], uint8_t chunk[25], bool& eof, int& counter, int& corrected);
void decodeBert(const float soft[368], uint8_t bits197[197], int& corrected);   // the PRBS9 bits

} // namespace m17
} // namespace dect2
