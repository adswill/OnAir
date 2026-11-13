// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// D-STAR protocol pieces (JARL D-STAR specification, DV mode): the radio header and its FEC, the sync patterns, the slow data, and the
// parsers for the GPS text (NMEA and D-PRS) the slow data carries. Every byte goes on the air least significant bit first.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace dect2 {
namespace dstar {

constexpr int kHeaderBytes = 41;         // 3 flags, RPT2, RPT1, UR, MY (8 each), MY suffix (4), CRC (2)
constexpr int kHeaderBits = 660;         // 328 bits + 2 tail bits, convolutional r = 1/2
constexpr int kFrameBits = 96;           // a voice frame: 72 AMBE bits + 24 slow data bits, 20 ms
constexpr int kVoiceBits = 72;
constexpr int kSuperframe = 21;          // frames from one data sync to the next (frame 0 carries the sync)

extern const uint8_t kFrameSync[15];     // after the bit sync, before the header: 111011001010000
extern const uint8_t kDataSync[24];      // slow data of frame 0: 0x55 0x2D 0x16
extern const uint8_t kEndPattern[48];    // in place of a voice frame at the end: 0x55 0x55 0x55 0x55 0xC8 0x7A
extern const uint8_t kSlowScramble[3];   // the slow data bytes of the other frames are sent XOR these: 0x70 0x4F 0x93

uint16_t crc(const uint8_t* p, size_t n);   // CRC-CCITT as D-STAR uses it (reflected 0x8408, start 0xFFFF, inverted); sent low byte first

struct Header {
    uint8_t flag[3] = {0, 0, 0};
    std::string rpt2 = "DIRECT", rpt1 = "DIRECT", ur = "CQCQCQ", my = "N0CALL", sfx = "";
};
void headerPack(const Header& h, uint8_t out[kHeaderBytes]);   // with the CRC
bool headerUnpack(const uint8_t in[kHeaderBytes], Header& h);   // false: the CRC does not match (h is filled anyway)
std::string flagText(const uint8_t f[3]);   // "data", "repeater", "emergency", "control" ...

// The header on the air: convolutional code (K = 3, 7 and 5), the 24-column interleaver, then the scrambler (x^7 + x^4 + 1, all ones)
void headerEncode(const uint8_t hdr[kHeaderBytes], uint8_t bits[kHeaderBits]);
// soft[i] > 0: bit i of the received header is a 1. Returns the channel bit errors the Viterbi decoder corrected.
int headerDecode(const float soft[kHeaderBits], uint8_t hdr[kHeaderBytes]);
void scrambleBits(uint8_t* bits, int n);    // the scrambler sequence, XOR (its own inverse)
void interleave(const uint8_t in[kHeaderBits], uint8_t out[kHeaderBits]);
void deinterleave(const float in[kHeaderBits], float out[kHeaderBits]);
void convEncode(const uint8_t* in, int nIn, uint8_t* out);     // 2 * nIn bits
int viterbi(const float* soft, int nOut, uint8_t* out);         // nOut decoded bits from 2 * nOut soft values; returns corrected bits

void bytesToBits(const uint8_t* b, int nBytes, uint8_t* bits);  // LSB first
void bitsToBytes(const uint8_t* bits, int nBytes, uint8_t* b);

// AMBE: the 72 voice bits as mbelib's ambe_fr[4][24] (the order DSD uses for D-STAR)
void ambeFrame(const uint8_t bits[kVoiceBits], char fr[4][24]);

// GPS text from the slow data
struct Fix {
    double lat = 0, lon = 0, altM = 0, speedKmh = 0;
    int course = 0;
    bool altValid = false, velValid = false;
    std::string call;          // D-PRS: the APRS source
    std::string comment;       // D-PRS: the text after the position
    bool aprs = false;
};
bool parseNmea(const std::string& line, Fix& f);   // $GPRMC / $GPGGA (and GN, GL ...), the checksum checked
bool parseDprs(const std::string& line, Fix& f);   // $$CRCxxxx,CALL>DEST,...:!DDMM.mmN/DDDMM.mmE...
uint16_t dprsCrc(const std::string& s);            // the CRC of a D-PRS line (the text after "$$CRCxxxx,")
std::string nmeaChecksum(const std::string& body); // two hex digits for "$<body>*hh"

} // namespace dstar
} // namespace dect2
