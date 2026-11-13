// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wenet: the high-speed image downlink of Project Horus balloons ("The Swift One", github.com/projecthorus/wenet, GPL-3.0-or-later).
// A transmitter (Raspberry Pi with an RFM98W / RFM22B / Si4032 on the 70 cm band) sends 2-FSK at 115.2 kbaud (deviation about +-72 kHz, the
// signal is about 200 kHz wide) or, from a sound-card ("I2S") transmitter, 96 kbaud. Packets of 256 bytes (the first byte is the type: 0x55 is
// an SSDV packet, 0x56 idle, 0x00 text, 0x01 .. 0x05 telemetry, 0x54 image telemetry) are framed as
//     preamble: 16 x 0x55 | unique word 0xABCDEF01 | 256 bytes + CRC-16 (CCITT, init 0xFFFF, low byte first) + 65 bytes of LDPC parity
// The LDPC code is a rate 0.8 repeat-accumulate code (2064 information bits, 516 parity bits, designed by Bill Cowley VK5DSP and David Rowe VK5DGR).
// On the 115.2 kbaud link every byte is sent as a serial word (start bit 0, 8 data bits LSB first, stop bit 1); on the 96 kbaud link the bits go
// out raw, MSB first, XORed with a fixed 125-byte sequence. This file is the receive side written for OnAir: the code tables are taken from the
// Wenet repository, the LDPC decoder, the demodulator and the sync search are new code. See docs/ssdv.md and THIRD_PARTY.md.
#pragma once
#include "dect2/common/ring.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dect2 {
namespace wenet {

constexpr int kPayloadBytes = 256;
constexpr int kInfoBytes = 258;          // payload + CRC
constexpr int kParityBytes = 65;         // 516 bits, padded
constexpr int kInfoBits = 2064;
constexpr int kParityBits = 516;
constexpr int kCodeBits = kInfoBits + kParityBits;   // 2580
constexpr int kBodyBytes = kInfoBytes + kParityBytes; // 323
constexpr int kUartBaud = 115200;
constexpr int kRawBaud = 96000;

extern const uint16_t kHrows[516 * 12];  // wenet_tables.cpp
extern const uint8_t kScramble[125];

uint16_t crc16(const uint8_t* d, size_t n);

// parity = the 65 parity bytes for the 258 bytes payload + CRC
void ldpcEncode(const uint8_t* info258, uint8_t* parity65);
// soft: kCodeBits values, positive = a 1 bit, any scale (the decoder is min-sum). bits gets the kCodeBits decisions. True when every check is met.
bool ldpcDecode(const float* soft, uint8_t* bits, int maxIter, int* iterations);

enum class Framing { Uart, Raw };

// The bits on the air (0 / 1, first bit first) of one frame: preamble, unique word, payload + CRC + parity in the framing of the link.
std::vector<uint8_t> frameBits(const uint8_t* payload256, Framing f, int preambleBytes = 16);

struct Hit {
    std::array<uint8_t, kPayloadBytes> pkt;
    int baud = 0;
    bool uart = true;
    bool inverted = false;       // found with the two frequencies swapped
    int iterations = 0;          // LDPC iterations used (0: the hard decisions already met every check)
    float snrDb = 0;             // decision-level SNR of the frame's soft values
    float cfoHz = 0;             // carrier offset the AFC had followed
};

// IQ in, packets out. configure() with the sample rate (at least 700 kHz; 2 Msps or more is usual), setOffset() = where the carrier sits in the
// samples. A raw 115.2 kbaud signal and a 96 kbaud one are looked for at the same time (setMask picks one).
class Demod {
public:
    Demod();
    ~Demod();
    static constexpr int kMaskUart = 1, kMaskRaw = 2;
    bool configure(double rateHz);
    bool ready() const;
    void setOffset(double hz);
    void setMask(int m);
    void reset();
    void feed(const cf32* x, size_t n);
    void setCallback(std::function<void(const Hit&)> cb);
    uint64_t goodFrames() const;
    uint64_t badFrames() const;     // a unique word was followed by a frame that failed the LDPC / CRC check
    double afcHz() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace wenet
} // namespace dect2
