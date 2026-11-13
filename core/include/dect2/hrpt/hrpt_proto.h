// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HRPT / AHRPT, the layers above the demodulator (both directions: the test signal is built with the same code it is checked by).
//
// MetOp AHRPT and FY-3 AHRPT: QPSK, CCSDS convolutional code (K = 7, r = 1/2, G1 = 171, G2 = 133 octal, G2 inverted), 1024-byte CADUs
// with the sync word 0x1ACFFC1D, CCSDS randomiser and Reed-Solomon (255,223) interleave 4: the layers meteor_proto.h already has.
// MetOp AVHRR/3 lines are CCSDS source packets of APID 103 (channel 3A) or 104 (3B): 12960 bytes of user data, a CDS time stamp
// (days since 1958, ms of day, us), and from byte 14 on 10-bit words (MSB first); the line's five channels, interleaved, start at word 55.
//
// NOAA HRPT: BPSK, Manchester, 665.4 kbit/s, minor frames of 11090 words of 10 bits (6 a second): 6 sync words, ID and time code,
// telemetry and the AVHRR line (2048 x 5 words interleaved from word 750).
#pragma once
#include "hrpt_tel.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2::hrpt {

constexpr int kAvhrrUserBytes = 12960;
constexpr int kAvhrrDataByte = 14;          // 10-bit words start here in the user data
constexpr int kAvhrrFirstWord = 55;         // first image word
constexpr double kLineMs = 1000.0 / 6.0;    // the AVHRR scans six lines a second
constexpr int kApid3a = 103, kApid3b = 104;
constexpr int kNoaaWords = 11090, kNoaaSyncWords = 6, kNoaaImageWord = 750;
extern const uint16_t kNoaaSync[kNoaaSyncWords];   // 0x284 0x16F 0x35C 0x19D 0x20F 0x095

// One line: ten-bit pixels, channel major
struct Line {
    uint32_t day = 0, ms = 0;
    std::vector<uint16_t> px;                 // kHrptChannels * kHrptWidth
    Line() : px((size_t)kHrptChannels * kHrptWidth, 0) {}
};

// 10-bit words, MSB first, to and from bytes
void packWords(const uint16_t* w, size_t n, uint8_t* out);        // out holds ceil(10 n / 8) bytes, zeroed by the caller
void unpackWords(const uint8_t* in, size_t n, uint16_t* w);

bool decodeAvhrrPacket(const std::vector<uint8_t>& user, Line& out);   // false: too short
std::vector<uint8_t> encodeAvhrrPacket(const Line& l);

// 11090 words, sync words included
bool decodeNoaaFrame(const uint16_t w[kNoaaWords], Line& out);         // false: more than 12 of the 60 sync bits wrong
void encodeNoaaFrame(const Line& l, uint16_t w[kNoaaWords]);
uint64_t noaaSync60();                                                 // the 60 sync bits, first one most significant

// The convolutional encoder as CCSDS has it (G2 inverted): one bit in, two code bits out
struct CcsdsEncoder {
    uint8_t sr = 0;
    void push(int bit, uint8_t& a, uint8_t& b);
};

// Soft-decision Viterbi for the same code as meteor::Viterbi27 (polynomials 0x4F / 0x6D, soft values > 0 are a 1), made for 2 - 3 Msym/s:
// 16-bit metrics, butterflies, a fixed survivor buffer, traceback every 64 steps.
class Viterbi {
public:
    Viterbi() { reset(); }
    void reset();
    void push(int a, int b);
    std::vector<uint8_t>& out() { return out_; }       // decided bits, oldest first, about 96 bits late
    void flush();
private:
    void traceback(int emit, bool all);
    alignas(16) int16_t m_[64];
    uint8_t dec_[160][64];   // the survivor decision (0 / 1) of each state at each step
    int n_ = 0, sinceNorm_ = 0;
    std::vector<uint8_t> out_;
};

} // namespace dect2::hrpt
