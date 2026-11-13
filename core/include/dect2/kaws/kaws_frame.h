// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS framing (spec KAWS-SPEC-v0.1 section 5): callsign (base 40, 9 characters, 48 bits), grid (6 characters, 25 bits), text alphabet (6 bits), block packing
// (8-bit block header + payload + CRC-16), the beacon (short, full), chat message, ack and data frames, the voice superframe.
//
// Decisions where the spec is inconsistent or open (the commit message lists them too):
//  - Frame type is 3 bits for every structured frame (0 beacon-short, 1 chat, 2 beacon-full, 3 data); the spec has 2 bits for the short beacon.
//  - The short beacon is a normal block: block header (8) + version (2) + type (3) + callsign (48) + grid (25) + power (6) + fine position (12) + link quality (8)
//    + CRC-16 = 128 bits = P0 K. The spec's 9 status/flag bits are dropped to make room for the block header and the wider type.
//  - The ack is the only raw frame (no block header): sender (48), message id (4), bitmap (32), SNR (6), suggested profile (4), CRC-16 = 110 bits, zero padded to K.
//    The spec says 110 bits fit into the 104 usable bits of P0; they fit only without the block header, hence raw.
//  - The block index is 4 bits, so a message has at most 16 blocks (the spec also says 32 in the ARQ section); bitmap bits 16..31 stay zero.
//  - The 22-bit recipient hash is FNV-1a (32 bit) of the 9 padded characters, xor-folded to 22 bits (spec open item 8).
//  - The 6-bit alphabet lists 53 symbols in the spec; the remaining 11 codes are ; * & $ < > [ ] _ | ~ (my completion), lower case is folded to upper case.
#pragma once
#include "dect2/kris/kris_common.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::kaws {

using Bits = std::vector<uint8_t>;   // one bit per byte, MSB first

struct BitWriter {
    Bits bits;
    void put(uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) bits.push_back((uint8_t)((v >> i) & 1u)); }
    void pad(size_t total) { if (bits.size() < total) bits.resize(total, 0); }
};
struct BitReader {
    const Bits& b;
    size_t pos = 0;
    explicit BitReader(const Bits& bits, size_t at = 0) : b(bits), pos(at) {}
    uint64_t get(int n) { uint64_t r = 0; for (int i = 0; i < n; i++) { r = (r << 1) | (pos < b.size() ? (b[pos] & 1u) : 0u); pos++; } return r; }
    bool overrun() const { return pos > b.size(); }
};

// ---- callsign, grid, hash, text ----
constexpr const char* kCallAlphabet = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";   // 40 symbols, index 0 = space / pad
constexpr int kCallChars = 9;
bool callsignValid(const std::string& cs);                // 1..9 characters of the alphabet (lower case folded), not blank
uint64_t callsignEncode(const std::string& cs);           // base 40, first character most significant; 40^9 < 2^48. ~0 for an invalid callsign
std::string callsignDecode(uint64_t v);                   // trailing pad removed
uint32_t recipientHash(const std::string& cs);            // 22 bits
bool gridEncode(const std::string& g, uint32_t& v);       // AA00AA (A-R, A-R, 0-9, 0-9, A-X, A-X), 18 662 400 values < 2^25
std::string gridDecode(uint32_t v);

constexpr int kTextBits = 6;
int textCode(char c);                                     // 0..63 (lower case folded; an unknown character gives the code of '?')
char textChar(int code);                                  // '?' for the unused code positions

// ---- blocks ----
constexpr int kBlockHeaderBits = 8;
constexpr int kBlockCrcBits = 16;
constexpr int kBlockOverhead = kBlockHeaderBits + kBlockCrcBits;
constexpr int kMaxBlocks = 16;                            // 4-bit block index
// A block of exactly K bits: [msgId 4][index 4][payload zero padded to K - 24][CRC-16 over everything before it]. False if the payload does not fit.
bool makeBlock(int K, uint8_t msgId, uint8_t index, const Bits& payload, Bits& block);
// Checks the CRC and splits; payload is K - 24 bits (padding included).
bool parseBlock(int K, const Bits& block, uint8_t& msgId, uint8_t& index, Bits& payload);
// Split a message bit string over blocks of size K (the last is zero padded). 0 blocks if it needs more than 16.
std::vector<Bits> splitMessage(int K, uint8_t msgId, const Bits& msg);

// ---- frames ----
enum class FrameType : uint8_t { BeaconShort = 0, Chat = 1, BeaconFull = 2, Data = 3 };
constexpr int kVersion = 1;                               // 2-bit version field

struct BeaconShort {
    std::string call, grid;
    int powerStep = 0;          // 0..63, KAAL scale 0.5 dB steps from -40 dBm
    int fineLat = 0, fineLon = 0;   // 0..63 each, position inside the grid square
    int linkQuality = 0;        // 0..255
};
struct BeaconFull {
    std::string call, grid;
    double lat = 0, lon = 0;    // degrees
    int powerStep = 0;          // 8 bits, EIRP dBm = -40 + 0.5 x value
    int linkQuality = 0;
    std::string text;           // up to 10 six-bit characters (spec P1 text field)
};
struct Chat {
    std::string from, to;       // `to` is sent as a 22-bit hash unless firstMessage
    bool firstMessage = false;  // full 48-bit recipient
    bool utf8 = false;          // text is raw bytes (8 bits each) instead of 6-bit characters
    bool more = false;          // more-blocks flag
    uint8_t seq = 0, ackRef = 0;
    std::string text;           // up to 100 characters (or bytes)
    uint32_t toHash = 0;        // filled on decode when the recipient was a hash
};
struct Ack {
    std::string from;
    uint8_t msgId = 0;
    uint32_t bitmap = 0;        // bit i = block i received with a good CRC
    int snrDb = 0;              // -35 .. +28
    int suggestedProfile = 0;   // 0..7
};

Bits encodeBeaconShort(const BeaconShort& b);                 // message bits (the payload of block 0, 104 bits)
bool decodeBeaconShort(const Bits& payload, BeaconShort& b);
Bits encodeBeaconFull(const BeaconFull& b);
bool decodeBeaconFull(const Bits& payload, BeaconFull& b);
Bits encodeChat(const Chat& c);                               // the message bits (401 for 50 six-bit characters)
bool decodeChat(const Bits& msg, Chat& c);
Bits encodeAck(const Ack& a, int K);                          // raw frame zero padded to K bits
bool decodeAck(const Bits& frame, Ack& a);                    // checks the CRC and the padding
FrameType frameTypeOf(const Bits& payload);                   // the 3 bits after the version (payload of block 0)
// Total message length in bits of the frame whose first block payload is `p0` (reads the length fields), or 0 when p0 is too short or not a frame.
size_t expectedMessageBits(const Bits& p0);
Bits encodeData(const std::vector<uint8_t>& bytes);           // version, type 3, length 16, bytes, CRC-24 over the bytes
bool decodeData(const Bits& msg, std::vector<uint8_t>& bytes);

// A message or the whole of block 0: which kind of block did we get?
enum class BlockKind { Invalid, Structured, Ack };
BlockKind classifyBlock(int K, const Bits& block);

// ---- voice superframe (P6, P7): 15 frames of (codec bits + 12 side bits) = K bits ----
constexpr int kVoiceFrames = 15;
constexpr int kVoiceSideBitsPerFrame = 12;
constexpr int kVoiceSideBits = kVoiceFrames * kVoiceSideBitsPerFrame;   // 180
int voiceCodecBits(int profile);                              // 28 (P6), 48 (P7), 0 otherwise
struct VoiceSide {
    std::string call;
    uint8_t callNumber = 0;     // 0..15 in the 8-bit header [version 2][type 2][call number 4]
    Bits slowData;              // 108 bits (text or position slice)
};
Bits voiceSideEncode(const VoiceSide& s);                     // 180 bits: call 48, header 8, slow data 108, CRC-16
bool voiceSideDecode(const Bits& side, VoiceSide& s);
// frames[i] = codec bits of frame i (voiceCodecBits(profile) each); side = 180 bits. K = 15 x (codec + 12) bits.
bool voiceBuild(int profile, const std::vector<Bits>& frames, const Bits& side, Bits& block);
bool voiceParse(int profile, const Bits& block, std::vector<Bits>& frames, Bits& side);

}  // namespace dect2::kaws
