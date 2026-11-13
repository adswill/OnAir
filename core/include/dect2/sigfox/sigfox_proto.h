// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sigfox uplink frame layer, from the public "Sigfox Connected Objects: Radio Specifications" (EP-SPECS Rev. 1.3, February 2019, published by
// Sigfox on build.sigfox.com). Receive side only: nothing here can make a message the network would accept (the authentication code needs the
// device's secret key, which is never known here and never checked).
//
// One frame on the air (bits, MSB first, left to right):
//   UL-Pr  19 bits 1010101010101010101
//   FT     13 bits frame type: names the container size (8, 9, 12, 16, 20 bytes) and the rank of the frame (first, second, third) (Table 3-3)
//   UL-PHY-CONTENT: the convolution code of the rank (1; 1 + X + X^2; 1 + X^2) applied to UL-CONTAINER + UL-CRC
//     UL-CONTAINER: LI 2 bits, BF 1 bit (1: the device asks for a downlink), REP 1 bit (always 0), MC 12 bits (message counter),
//                   ID 32 bits (the device ID with its bytes in reverse order), UL-PAYLOAD 0..12 bytes, UL-AUTH 2..5 bytes (Table 3-1)
//     UL-CRC: 16 bits, X^16 + X^12 + X^5 + 1 over UL-CONTAINER, remainder XORed with 0xFFFF
// A message is sent as one frame or as three (ranks 1, 2, 3) on different frequencies; all three carry the same container.
//
// Not stated in the public text, so not certain here: the CRC register's start value (0 is used: it reproduces the one public example frame we
// have, see tests/sigfox/test_sigfox_proto.cpp), the convolution coder's start state (0 is assumed), and which bit value is a phase change in the
// D-BPSK (the receiver tries both).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::sigfox {

constexpr int kPreambleBits = 19;
constexpr int kFtBits = 13;

struct FrameType { uint16_t ft; int container; int rank; bool control; };
const FrameType* frameTypes(int& count);                   // Table 3-3
const FrameType* frameTypeOf(uint16_t ft, int maxErrors = 0);   // the entry within maxErrors bit errors (nullptr: none, or two equally close)
uint16_t frameTypeFor(int container, int rank, bool control = false);   // 0: no such entry

// Table 3-1: the payload length of a container size and length indicator; -1: not a valid pair. singleBit: a 1 bit message (the bit is li & 1).
int payloadBytes(int container, int li, bool& singleBit);
int authBytes(int container, int li);
// the container size and LI for a payload (0..12 bytes; singleBit: a 1 bit message of value bitValue, payload empty)
bool layoutFor(int payloadLen, bool singleBit, bool bitValue, int& container, int& li);

uint16_t crc16(const std::vector<uint8_t>& bytes);         // UL-CRC over the container bytes

// The convolution code of a rank (1: identity, 2: 1 + X + X^2, 3: 1 + X^2) on a bit sequence, and its inverse
std::vector<uint8_t> convEncode(const std::vector<uint8_t>& bits, int rank);
std::vector<uint8_t> convDecode(const std::vector<uint8_t>& bits, int rank);

struct Uplink {
    int rank = 1, container = 0, li = 0;
    uint16_t ft = 0;
    bool control = false;          // a control message frame type (keep-alive and the like)
    bool downlinkRequest = false;  // BF
    bool rep = false;              // REP (always 0 in the specification)
    bool singleBit = false, bitValue = false;
    uint32_t mc = 0;               // message counter, 12 bits
    uint32_t id = 0;               // device ID (bytes put back in order)
    std::vector<uint8_t> payload, auth;
    uint16_t crc = 0;
    bool crcOk = false;
};

// The bits of one frame (preamble, frame type, coded content). auth: the authentication bytes to put in (a test signal cannot compute real ones);
// when empty, made-up bytes are used.
std::vector<uint8_t> buildFrame(uint32_t id, uint32_t mc, const std::vector<uint8_t>& payload, int rank, bool downlinkRequest = false,
                                bool singleBit = false, bool bitValue = false, const std::vector<uint8_t>& auth = {});

// Reads a frame whose frame type starts at bits[pos] (the preamble before it): the frame type (up to 1 bit error), the content decoded by its
// rank, and the CRC. True only when the CRC matches and the length indicator is valid for the container.
bool parseFrame(const std::vector<uint8_t>& bits, size_t pos, Uplink& out);
// The number of bits after the preamble for a container size
inline int frameBitsAfterPreamble(int container) { return kFtBits + 8 * container + 16; }

std::string idHex(uint32_t id);       // 8 hex digits
std::string hexOf(const std::vector<uint8_t>& b);

// The regional profiles of the public specification: uplink centre frequency and the uplink bit rate the profile uses
struct Zone { const char* name; double ulMhz, dlMhz; int baud; const char* where; };
const Zone* zones(int& count);

}  // namespace dect2::sigfox
