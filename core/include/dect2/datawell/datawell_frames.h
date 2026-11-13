// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bit stream analysis for the Datawell buoy HF link: nothing about the real frame format is public, so this only finds structure that is
// there: a repetition period, a constant pattern at the start of each period, and which standard CRC (if any) the periods agree with.
// Everything it reports is a measurement of the bits, never knowledge of the Datawell format.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::datawell {

struct CrcSpec { const char* name; int width; uint32_t poly, init, xorout; bool refin, refout; };
const CrcSpec* crcSpecs(int& count);                              // 16 and 32 bit CRCs, then 8 bit ones (weak: too easy to match by chance)
uint32_t crcCompute(const CrcSpec& s, const uint8_t* d, size_t n);

struct Period { int lag = 0; double z = 0; };                     // the lag with the most agreement between bit i and bit i + lag
Period findPeriod(const std::vector<uint8_t>& bits, int minLag, int maxLag);

struct Sync { bool found = false; int start = 0, len = 0; std::string hex; std::vector<uint8_t> bits; };
// the longest run of columns (mod lag) where every period carries the same bit; start is a bit index into `bits` (first column of the run)
Sync findSync(const std::vector<uint8_t>& bits, int lag);

struct CrcHit { std::string name; int pass = 0, total = 0; bool inverted = false, lsbFirst = false, crcLittle = false; int skipBytes = 0, specIdx = -1; };
// Frames of equal length (bits, starting at the sync pattern, syncBits long): the CRC attempt that most frames agree with. Needs a pass
// count of at least 3 and half the frames (8 bit checks: all of at least 8) before it reports a name.
CrcHit bestCrc(const std::vector<std::vector<uint8_t>>& frames, int syncBits);
// does one frame pass the given attempt
bool frameCrcOk(const std::vector<uint8_t>& frameBits, const CrcHit& h);

std::string bitsToHex(const uint8_t* bits, size_t n, bool lsbFirst, bool invert);

} // namespace dect2::datawell
