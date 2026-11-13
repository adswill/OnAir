// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteosat High Rate DCP (HRDCP) message format and coding, from EUMETSAT TD 16 "Meteosat Data Collection and Distribution Service"
// (EUM/OPS/DOC/08/0325 v2B, sections 3.2 - 3.4 and 5.3). Both directions: the test signal is built with the same code it is checked by.
//
//   Message  : 12-byte header (31-bit BCH platform address and a reserved bit set to 1, 16-bit platform data length, 16-bit sequence counter,
//              engineering word: 3-bit version (1), 1-bit type (0 self-timed, 1 alert), 2-bit compression (0 none, 1 gzip), 10-bit health;
//              16 spare bits), the platform data (1 - 65535 bytes, any format) and a CRC-32 (polynomial 0x741B8CD7, initial value 0, MSB first)
//              over header and data, MSB first. Every field is sent MSB first.
//   Coding   : zero filled to a whole number of 669-byte frames, each Reed-Solomon (255,223) coded with interleave 3 (CCSDS 131.0-B, dual basis) to
//              765 bytes, the CCSDS pseudo-randomiser over the coded bytes, one tail byte 0x80 (repeated during the ramp down), then the CCSDS
//              convolutional code r = 1/2 K = 7 (G1 = 171, G2 = 133 octal, G2 inverted).
//   On air   : 2 s unmodulated carrier (at 45 degrees), 128-bit preamble (0xA05050A0 four times) and the 64-bit marker 0x034776C7272895B0 as
//              "BPSK" at 1200 bit/s (0 at 45 degrees, 1 at 225 degrees), then the code symbols as offset QPSK at 2400 bit/s (G1 on I, /G2 on Q half a
//              symbol later; 0 is +1, 1 is -1), root raised cosine roll-off 0.5 throughout. 2.25 kHz wide, assigned on the 1.5 kHz grid of table 11.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace hrdcp {

constexpr double kSymbolRate = 1200.0;            // per rail, in both the BPSK and the OQPSK parts
constexpr double kCarrierSec = 2.0;
constexpr int kPreambleBits = 128, kAsmBits = 64;
constexpr uint32_t kPreambleWord = 0xA05050A0u;
constexpr uint64_t kAsm = 0x034776C7272895B0ull;
constexpr int kHeaderBytes = 12, kCrcBytes = 4;
constexpr int kFrameData = 669;                   // 3 x 223: the message bytes one coded frame carries
constexpr int kFrameCoded = 765;                  // 3 x 255
constexpr uint8_t kTail = 0x80;
constexpr int kMaxData = 65535;

uint32_t crc32(const uint8_t* p, size_t n);       // 0x741B8CD7, initial 0, MSB first, no final xor ("CatMouse987654321" -> 0x1FC0DFEC)

struct Header {
    uint32_t address = 0;                         // the 31-bit BCH address word (dcs::addressWord), b1 first
    bool reserved = true;                         // sent as 1
    uint16_t length = 0;                          // platform data bytes
    uint16_t sequence = 0;
    int version = 1, type = 0, compression = 0, health = 0;
    uint16_t spare = 0;
};
void putHeader(const Header& h, uint8_t out[kHeaderBytes]);
Header getHeader(const uint8_t in[kHeaderBytes]);

// The message (header, data, CRC) for a header whose length field is set from data
std::vector<uint8_t> buildMessage(Header h, const std::vector<uint8_t>& data);
int framesFor(size_t messageBytes);               // coded frames that carry a message of this size

// Message -> the bits that go into the convolutional encoder: randomised Reed-Solomon frames, then the tail byte(s)
std::vector<uint8_t> codedBytes(const std::vector<uint8_t>& message, int tailBytes = 1);
// Bits (MSB first) -> code symbols: for each bit the G1 bit (I rail) and the inverted G2 bit (Q rail). The encoder starts in the zero state.
void convEncode(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& iBits, std::vector<uint8_t>& qBits);

// Reed-Solomon decoding of one 765-byte coded frame (derandomised first): the 669 data bytes; corrected gets the symbol errors fixed. The randomiser
// runs on through the frames, but 765 bytes are three of its 255-byte periods, so every frame sees the same sequence.
bool decodeFrame(const uint8_t coded[kFrameCoded], uint8_t data[kFrameData], int& corrected);

struct Decoded {
    bool headerOk = false;                        // the first frame decoded and its header looks right (address passes the BCH check, version 1)
    bool complete = false;                        // every frame decoded
    bool crcOk = false;
    Header hdr;
    int addressFixed = 0;                         // bit errors the BCH code corrected in the address
    int frames = 0, framesBad = 0, rsCorrected = 0;
    std::vector<uint8_t> data;                    // the platform data (length from the header, what could be decoded)
};
// The decoded info bytes after the marker (Viterbi output, MSB first) -> the message. Returns the frames still missing when bytes is short (0 when done).
int decodeMessage(const std::vector<uint8_t>& infoBytes, Decoded& out);

std::string healthText(int health);               // the 10 health bits as binary
std::string describe(const Decoded& d);           // one line

} // namespace hrdcp
} // namespace dect2
