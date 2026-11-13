// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NXDN common air interface (NXDN TS 1-A): the parts both the receiver and the test signal use.
//
// A frame is 192 four-level symbols (384 bits): the 10-symbol frame sync word (FSW), then 182 symbols scrambled by PN9 (x^9 + x^5 + 1,
// seed 0xE4; a 1 inverts the symbol): the 8-symbol LICH (link information, one bit per symbol, even parity) and 174 symbols of channels.
// NXDN48 sends 2400 symbols/s (80 ms frames, +-1050 Hz outer deviation), NXDN96 4800 symbols/s (40 ms, +-2400 Hz).
//
// The channels are protected by the K=5 convolutional code (G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4), punctured and block interleaved:
//   SACCH   26 bits (+ CRC-6)  -> 72 coded -> 60 on air (12 x 5 interleaver); 4 in a superframe carry one 72-bit message
//   FACCH1  80 bits (+ CRC-12) -> 192      -> 144       (16 x 9)
//   CAC    155 bits (+ CRC-16) -> 350      -> 300       (12 x 25)
// A traffic frame (RTCH) is LICH, SACCH and either four 72-bit AMBE+2 voice frames (VCH) or two FACCH1, or one of each per half.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace nxdn {

constexpr int kFrameSym = 192, kFswSym = 10, kPayloadSym = 182, kLichSym = 8;
extern const int kFsw[kFswSym];                 // -3 +1 -3 +3 -3 -3 +3 +3 -1 +3 (0xCDF59)
inline int dibitToSym(int d) { return d == 1 ? 3 : d == 0 ? 1 : d == 2 ? -1 : -3; }   // 01 +3, 00 +1, 10 -1, 11 -3
void pn9(uint8_t* out, int n);                  // the scrambler bits for the 182 symbols after the FSW

// LICH: RF channel type (0 RCCH control, 1 RTCH traffic, 2 RDCH data, 3 composite), functional channel type, option, direction (1 outbound)
enum { kRcch = 0, kRtch = 1, kRdch = 2 };
enum { kFctCac = 0, kFctSacchNs = 0, kFctSacchSf = 2 };
// RTCH option (which half carries what): 0 FACCH1 + FACCH1, 1 FACCH1 + voice, 2 voice + FACCH1, 3 voice + voice
struct Lich { int rfct = 0, fct = 0, opt = 0, dir = 0; };
uint8_t lichEncode(const Lich& l);             // 8 bits, MSB first, parity last
bool lichDecode(uint8_t v, Lich& l);           // false: parity wrong

uint32_t crc6(const uint8_t* bits, int n);
uint32_t crc12(const uint8_t* bits, int n);
uint32_t crc16(const uint8_t* bits, int n);

// One of the coded channels
enum class Ch { Sacch, Facch1, Cac };
int infoBits(Ch c);                             // 26, 80, 155 (before the CRC)
int airBits(Ch c);                              // 60, 144, 300
void encode(Ch c, const uint8_t* info, uint8_t* air);
// soft: > 0 a 1, < 0 a 0, magnitude the confidence. Returns true when the CRC holds; info gets infoBits(c) bits, errs the channel bits
// that differ from the re-encoded result (when the CRC holds)
bool decode(Ch c, const float* soft, uint8_t* info, int* errs = nullptr);

// Layer 3 messages (the first byte: 2 flag bits, 6-bit message type)
enum : int { kVcall = 0x01, kTxRel = 0x08, kIdle = 0x10, kSiteInfo = 0x18, kSrvInfo = 0x19, kStatReq = 0x32, kSdHeader = 0x38, kSdData = 0x39 };
struct Msg {
    int type = -1;
    int callType = 1;            // 0 broadcast, 1 group (conference), 4 individual, 6 interconnect
    int option = 0;              // voice call option (5 bits: duplex, transmission mode)
    int src = 0, dst = 0;        // radio unit id, talkgroup or unit
    int cipher = 0, keyId = 0;
    int status = 0;              // STAT_REQ
    int block = 0, blocks = 0;   // short data: block number, block count (header)
    std::string text;            // short data: 8 characters per block
    uint32_t location = 0;       // SITE_INFO / SRV_INFO: the 24-bit location id
    int service = 0, channel = 0;
};
// bytes: MSB first; returns the bit count used
int buildMsg(const Msg& m, uint8_t* bits, int maxBits);
bool parseMsg(const uint8_t* bits, int nBits, Msg& m);
const char* msgName(int type);
const char* callTypeName(int t);
inline int locSystem(uint32_t loc) { return (int)((loc >> 10) & 0xFFF); }   // the location id as the regional category splits it
inline int locSite(uint32_t loc) { return (int)(loc & 0x3FF); }

// AMBE+2 order: the 72 voice bits of a VCH into mbelib's ambe_fr[4][24]
void ambeFrame(const uint8_t bits[72], char fr[4][24]);

} // namespace nxdn
} // namespace dect2
