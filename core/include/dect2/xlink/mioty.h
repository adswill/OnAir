// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// mioty uplink (ETSI TS 103 357 V1.1.1, TS-UNB, UL-ULP mode, standard TSMA mode with a 2 380.371 Hz carrier spacing): a radio frame is split into 24 core radio-bursts
// (plus one extension burst per payload byte beyond 20) of 36 (G)MSK symbols at 2 380.371 sym/s, spread over time and over 24 carriers by a TSMA pattern.
//
// Receive chain: pilot-sequence search over the whole 100 kHz channel (FFT of each 12-symbol window against the pilot), a pattern search that fits the 17 uplink TSMA
// patterns to the detected bursts, coherent demodulation of every burst of the fitted pattern, de-interleaving (clause 6.4.4.6), rate 1/3 K = 7 Viterbi, PN9 de-whitening,
// the header and payload CRC-8 (clause 6.4.6.2), then the extension frame from the PHY header (clause 6.4.7.1.6.2).
// MAC fields of the fixed MAC mode are shown (flags, short address or EUI-64, packet counter); the MAC payload is encrypted with the network key and stays encrypted.
//
// Not implemented: the downlink, the optional sync-burst, UL-ER mode (396.729 sym/s), the narrow and wide TSMA modes, coherent combining of repeated frames.
// Points the specification leaves open, chosen here and marked in the code: the bit numbering of the MAC header (bit 0 = least significant), the shift direction of the
// extension frame's Galois LFSR (right shift), the convolutional code's tap order (as the IEEE 802.11 code: the octal value's top bit is the newest input bit).
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::xlink::mioty {

constexpr double kSymRate = 2380.371;
constexpr double kCarrierStep = 2380.371;
constexpr int kSps = 48;
constexpr double kRate = kSymRate * kSps;      // the receiver's working rate, about 114.26 kHz
constexpr int kCore = 24;

// pattern index 0 .. 16: UPG1 patterns 1 - 8, UPG2 patterns 1 - 8, UPG3
int patternCarrier(int pattern, int burst);    // burst 0 .. 23
int patternTime(int pattern, int burst);       // burst 1 .. 23: symbols between the pilot centres of bursts burst - 1 and burst
int patternGroupSpacing(int pattern);          // TUPGx of Table 6-46
const char* patternName(int pattern);          // "UPG1/3" and so on

uint8_t crc8(const std::vector<uint8_t>& bits);           // CRC-8 0x9B, initial 0xFF, MSB first (clause 6.4.6.2)

struct Frame {
    int pattern = 0;
    int psi = 0;                 // MPDU bytes
    int mmode = 0;
    std::vector<uint8_t> mpdu;
    uint8_t headerCrc = 0, payloadCrc = 0;
    double freqHz = 0;           // carrier 0 of the frame, relative to the working band's centre
    double snrDb = 0;
    int burstsFound = 0;         // bursts the pattern search matched (of 24)
    int64_t at = 0;              // sample (working rate) of burst 0's pilot centre
};

// MAC fields of the fixed MAC mode (clause 6.3.2.2) as text
std::string describeMpdu(const std::vector<uint8_t>& mpdu);

// Test signal: one uplink frame at sample rate fs, burst 0's pilot centre at startSample, carrier 0 at f0Hz (relative to 0 Hz).
std::vector<cf32> txFrame(const std::vector<uint8_t>& mpdu, int pattern, double fs, double f0Hz, int64_t startSample, size_t totalSamples, int mmode = 0);
void addFrame(std::vector<cf32>& buf, const std::vector<uint8_t>& mpdu, int pattern, double fs, double f0Hz, int64_t startSample, int mmode = 0);

class Receiver {
public:
    using Cb = std::function<void(const Frame&)>;
    Receiver();
    ~Receiver();
    void setCallback(Cb cb);
    void reset();
    void feed(const cf32* x, size_t n);          // complex samples at kRate
    void flush();                                // end of input: look at what is left
    uint64_t pilots() const;                     // radio-bursts detected (all, before the pattern search)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::xlink::mioty
