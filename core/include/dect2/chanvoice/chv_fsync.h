// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Kenwood FleetSync data burst on an analog FM voice channel: 1200 bit/s FFSK (mark = bit 1 = 1200 Hz, space = bit 0 = 1800 Hz, plain NRZ), an
// alternating preamble of at least 24 bits, the 16 bit sync word 0xA23E, 4 lead-in bits, then two 32 bit words; the low 16 bits of the second word
// are a block check. FleetSync II repeats the data as four 64 bit blocks of four 15 bit single-error-correcting code words each. The fleet number
// is byte 2 of word 1 plus 99, the unit ID is byte 3 of word 1 shifted left 4 bits plus the high nibble of word 2, plus 999.
//
// What is defined here is what the public description of the format gives: the framing, the block check, the FleetSync II code and the fleet / unit
// fields. The meaning of the other bits (message type, status, text, position) is not published in that description and is NOT decoded: the two raw
// words are reported as they are.
//
// Source: docs/fleetsync.md and internal/radio/fleetsync of GopherTrunk by Matt Cheramie (Apache-2.0, github.com/MattCheramie/GopherTrunk), whose
// format description (sync word, block check polynomial 0x6815, FleetSync II parity checks) was used to write this code; no code is copied, it is a
// fresh C++ implementation (credit in THIRD_PARTY.md).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace chv {

struct FsyncMessage {
    int fleet = 0;                 // fleet number (word 1 byte 2 + 99)
    int unit = 0;                  // unit ID (+ 999)
    bool fs2 = false;              // decoded through the FleetSync II error correction
    uint32_t word1 = 0, word2 = 0; // the two data words as received (word 2 low 16 bits = the block check)
    double timeSec = 0;            // where the burst ended in the audio fed so far
};

uint16_t fsyncCrc(uint32_t word1, uint32_t word2);   // the block check over the two words (word 2 bits 15..1 count only as parity)
// Decode the bits that follow the sync word (one bit per byte). 68 bits are enough for FleetSync, 260 for FleetSync II. True when a block check holds.
bool fsyncDecodeBits(const uint8_t* bits, size_t n, FsyncMessage& out);
std::string fsyncText(const FsyncMessage& m);        // "FleetSync fleet 107 unit 1772"

// The bits after the sync word (68 bits, or 260 for FleetSync II). `head` = word 1 bits 31..16 and `mid` = word 2 bits 27..16 are carried as they are (they
// are not interpreted).
std::vector<uint8_t> fsyncPayloadBits(int fleet, int unit, bool fs2, uint16_t head = 0, uint16_t mid = 0);
// The burst as audio (full scale = amp): `leadBits` alternating bits, the sync word, the payload, then `tailBits` of mark.
std::vector<float> fsyncEncode(int fleet, int unit, bool fs2, double rate, double amp = 0.5, int leadBits = 48, bool invert = false, uint16_t head = 0, uint16_t mid = 0);

class FsyncDecoder {
public:
    explicit FsyncDecoder(double rate = 48000.0);
    void setCallback(std::function<void(const FsyncMessage&)> cb) { cb_ = std::move(cb); }
    void reset();
    void feed(const float* x, size_t n);
    uint64_t messages() const { return count_; }
    uint64_t crcFailures() const { return crcBad_; }   // bursts whose sync was found but whose block check did not hold (not reported)
    bool busy() const;                                   // a sync word was found and its frame is being read

private:
    static constexpr int kPhases = 8;
    static constexpr int kFrameBits = 260;
    struct Phase {
        uint64_t reg = 0;
        double next = 0;
        bool capture = false, inverted = false;
        int got = 0;
        uint8_t buf[kFrameBits];
    };
    double rate_, spb_;
    int len_;
    std::vector<float> ring_;
    size_t wr_ = 0;
    uint64_t n_ = 0;
    std::vector<float> c12_, s12_, c18_, s18_;
    Phase ph_[kPhases];
    std::function<void(const FsyncMessage&)> cb_;
    uint64_t count_ = 0, crcBad_ = 0;
    FsyncMessage last_; double lastEnd_ = -1;
};

} // namespace chv
} // namespace dect2
