// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench: working on a recovered bit stream (one bit per byte, 0 or 1): the usual line-code and bit-order transforms, text views,
// pattern search and export.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace wb {

using Bits = std::vector<uint8_t>;

struct BitOpts {
    bool invert = false;          // 0 <-> 1
    bool diffDecode = false;      // out[i] = in[i] xor in[i - 1] (differential decoding)
    bool nrzi = false;            // NRZI decode: 0 = the level changed, 1 = it did not (HDLC / USB convention; with invert, the other way round)
    int manchester = 0;           // 0 off, 1 IEEE 802.3 (a pair "10" is 0... see below), 2 G.E. Thomas (the opposite)
    int manchesterPhase = 0;      // 0 or 1: which bit of the stream starts a pair
    bool bitReverse = false;      // reverse the bit order inside each byte (MSB <-> LSB first)
    size_t skip = 0;              // drop this many raw bits first (the byte alignment)
};
// Order: skip, invert, Manchester, NRZI, differential, bit reversal. Manchester: IEEE 802.3 reads "01" as 1 and "10" as 0 (a rising edge
// is 1); G.E. Thomas the other way round. Pairs "00" and "11" are dropped as errors (counted in *errors when given).
Bits applyBits(const Bits& in, const BitOpts& o, size_t* errors = nullptr);

enum ByteView { ViewHex = 0, ViewBinary, ViewAscii };
// bits -> bytes, MSB first unless lsbFirst; a trailing partial byte is dropped
std::vector<uint8_t> packBits(const Bits& b, bool lsbFirst = false);
// A text view: width bytes per line, each line starting with its bit offset; ASCII shows printable bytes and '.' for the rest
std::string formatBits(const Bits& b, int view, int bytesPerLine = 16, bool lsbFirst = false, size_t maxBytes = 1u << 16, size_t firstByte = 0);

// A pattern: "1011 0010", "0x7E", "0b1010", "A5F0" (hex) are read; false when it is not one. Spaces and '_' are ignored.
bool parsePattern(const std::string& text, Bits& out);
// Where the pattern starts (bit indices); allowInverted also finds the inverted pattern (inv gets its starts), maxMismatch bits may differ
std::vector<size_t> findPattern(const Bits& b, const Bits& pat, bool allowInverted = false, int maxMismatch = 0, size_t limit = 100000, std::vector<size_t>* inverted = nullptr);

bool writeBitsText(const std::string& path, const Bits& b, std::string& err);    // "0101...": 64 bits per line
bool writeBitsBin(const std::string& path, const Bits& b, bool lsbFirst, std::string& err);
std::string bitsToString(const Bits& b, size_t from = 0, size_t n = 64);

} // namespace wb
} // namespace dect2
