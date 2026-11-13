// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Eurobalise telegram coding (UNISIG SUBSET-036 issue 4.0.0, clause 4.3 and Annex A / B2), the air gap that EBICab systems share in its
// physical form (a 27 MHz tele-powering field from the train, a 4.5 MHz band FSK up-link). Public and normative; the EBICab 700 / 900 telegram
// formats are not published and are not handled (docs/ebicab.md).
//
//  - telegram: 1023 bits (long, 830 user bits) or 341 bits (short, 210 user bits), sent endlessly while the train passes;
//  - 83 / 21 words of 11 bits (each is 10 scrambled user bits through a substitution table), 3 control bits, 12 scrambling bits, 10 extra shaping
//    bits and 85 check bits (a cyclic code: g(x) of degree 75 and f(x) of degree 10 for the start position);
//  - the receiver (4.3.4.1) takes a window of n + r bits, requires the parity check, the repeat of the first r bits at its end, a start position
//    from the f(x) syndrome, and 11-bit words that are all valid.
// Bit order: the first bit sent is b(n-1). Inversion of all bits is recognised.
//
// Interpretations that no published test vector could confirm are listed in docs/ebicab.md (the scrambler output, the header layout of SUBSET-026).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace ebicab {

constexpr double kBitRate = 564480.0;        // bit/s, +-2.5 %
constexpr double kFreqZeroHz = 3.951e6;      // '0' bits
constexpr double kFreqOneHz = 4.516e6;       // '1' bits
constexpr double kCentreHz = 4.234e6;
constexpr double kTelePowerHz = 27.095e6;    // the train's tele-powering carrier (amplitude modulated), not part of the up-link
constexpr int kLong = 1023, kShort = 341;

// SUBSET-026 telegram header (the first 50 user bits) as commonly published; "valid" is false when the user bits are too short
struct Header {
    bool valid = false;
    int qUpdown = 0, mVersion = 0, qMedia = 0, nPig = 0, nTotal = 0, mDup = 0, mMcount = 0, nidC = 0, nidBg = 0, qLink = 0;
    int firstPacket = -1;   // NID_PACKET of the first packet after the header (255: end of information), -1 when none
};
Header parseHeader(const std::vector<uint8_t>& userBits);

// SUBSET-026 chapter 7 packets after the header, one readable line each. Field layout read from the openETCS XML transcription of the chapter (a Baseline 3 text).
struct Packet { int nid = -1, qDir = 0, lenBits = 0; std::string text; };
std::vector<Packet> parsePackets(const std::vector<uint8_t>& userBits);

struct Telegram {
    bool longFmt = false;
    bool inverted = false;           // the whole telegram was received inverted (the radio side swapped the two frequencies)
    bool inversionBit = false;       // b109, which a correct balise sets to 0
    int controlBits = 0;             // b109 b108 b107 as sent (0b001 expected)
    bool formatKnown = true;         // b108 = 0 and b107 = 1
    std::vector<uint8_t> userBits;   // 830 or 210, first bit first
    uint16_t scrambling = 0;         // the 12 scrambling bits
    int shift = 0;                   // where the telegram started in the window
    Header header;
    std::vector<Packet> packets;
    std::string hex;                 // the user bits as hex (padded with zeros to a whole byte)
};

// Encoder (the test signal): userBits 830 (long) or 210 (short) bits; scrambling 0 .. 4095. Tries extra shaping bits until the alphabet / parsing
// conditions of 4.3.2.5.2 and .3 hold (the aperiodicity and under-sampling conditions are not checked: they only matter for the safety proof).
// Returns n bits, the first one sent first; empty when no extra shaping bits work for this scrambling value (the caller tries another).
std::vector<uint8_t> encode(const std::vector<uint8_t>& userBits, bool longFmt, uint16_t scrambling);
std::vector<uint8_t> userBitsFromHeader(const Header& h, int totalBits);   // header, then NID_PACKET 255 and zeros

// Decoder of one window of exactly n + r bits as the receiver of 4.3.4.1 reads it. Returns false when any check fails.
bool decodeWindow(const uint8_t* bits, size_t count, bool longFmt, Telegram& out);

// Streaming receiver: feed bits, get telegrams. A rolling parity check (one step per bit and format) tries every bit position at a low cost.
class Stream {
public:
    Stream();
    ~Stream();
    void reset();                    // a new burst: the 7500 bit rule starts again
    bool bit(int b);                 // true when a telegram was completed with this bit (it is in last())
    const Telegram& last() const;
    uint64_t bitsSeen() const;
    uint64_t parityHits() const;     // windows that passed the parity check (a later check may still fail)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace ebicab
} // namespace dect2
