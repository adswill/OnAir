// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ITU-T J.83 Annex B (North American cable, "clear QAM"): 64-QAM (5.056941 Msym/s, roll-off 0.18) and 256-QAM (5.360537 Msym/s, 0.12).
// Inner code: a rate-14/15 or 19/20 trellis over 5-symbol groups (punctured 16-state convolutional code on the LSBs, 90-degree invariant precoding).
// Outer: RS(128,122) over GF(128), convolutional interleaver, a randomizer over GF(128), and an FEC frame with a sync trailer. The transport packets are carried
// as a plain bit stream; the sync byte is replaced by a parity checksum.
// J83bDecoder eats equalised symbols, J83bEncoder makes test signals (nothing is transmitted).
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace j83b {

constexpr double kSymRate64 = 5.056941e6, kSymRate256 = 5.360537e6, kRollOff64 = 0.18, kRollOff256 = 0.12;

// constellation with unit mean energy: the point of label c (64: C5..C0, 256: C7..C0), and the bit positions of the coded bits
const std::vector<cf32>& points(bool qam256);
// the interleaver of a control word: I, J (false = reserved)
bool interleaver(int control, int& I, int& J);

// GF(128), primitive x^7 + x^3 + 1
int gfMul(int a, int b);
// RS(128,122): encode 122 symbols to 128; decode in place, returns the corrected symbols, or -1
void rsEncode(uint8_t* block128);
int rsDecode(uint8_t* block128);

struct DecStats {
    bool locked = false;                 // frame sync found and repeating
    int control = -1, I = 0, J = 0;
    int groupPhase = 0;
    bool swapAB = false;                 // the A and B bit streams turned out swapped in the label (see .cpp, the assignment is not spelled out in the text)
    uint64_t frames = 0, rsClean = 0, rsCorrected = 0, rsFailed = 0, framesBad = 0, packets = 0;
    uint32_t correctedSymbols = 0;
};

class J83bDecoder {
public:
    J83bDecoder();
    ~J83bDecoder();
    void configure(bool qam256);          // resets everything
    void reset();
    // Trial: true once the symbols handed in so far (at most about 100000) show a repeating sync trailer. The search covers the five positions of the trellis group and
    // both assignments of the A / B streams.
    bool trial(const cf32* sym, size_t n);
    void feed(const cf32* sym, size_t n);  // after trial() said true (or locked): decode, deliver packets through the callback
    void setPacketCallback(std::function<void(const uint8_t* pkts, size_t nPackets)> cb);
    const DecStats& stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

struct EncConfig {
    bool qam256 = false;
    int control = 0;    // 0 = I 128, J 1 (see interleaver())
    bool swapAB = false;
};

class J83bEncoder {
public:
    explicit J83bEncoder(const EncConfig& c);
    ~J83bEncoder();
    void push(const uint8_t* pkt188);     // transport packets, MSB first
    void take(std::vector<cf32>& symbols);    // the symbols finished so far (whole FEC frames)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace j83b
} // namespace dect2
