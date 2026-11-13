// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EDACS control channel (outbound) frame:
//  - 9600 baud GFSK; a 30 ms "bucket" of 288 bits = 48 sync bits (16 dotting 0x5555, a 16-bit field 0x5712 = five dotting bits + the 11-bit Barker word
//    11100010010, 16 dotting 0x5555: the pattern 0x555557125555) + two 28-bit messages, each with 12 BCH bits (40 bits), each sent three times with the
//    middle copy inverted (majority voting per bit). Framing from US 7,546,135 B2 and the DSD-FME / EDACS-FM sources (credited in THIRD_PARTY.md).
//  - Check bits: the binary BCH (40,28) code, a shortened BCH(63,51,t=2) over GF(2^6) with p(x) = x^6 + x + 1, generator
//    g(x) = x^12 + x^10 + x^8 + x^5 + x^4 + x^3 + 1 (0x1539): check = (data << 12) mod g, no initial fill, no final mask. Derived from the field and
//    cross-checked against the DSD-FME encoder on 2000 words (tests/edacs/test_edacs_msg.cpp holds the vectors).
// The data polarity is found from the Barker word / check bits (both are tried), so there is no setting for it.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <vector>

namespace dect2::edacs {

constexpr double kBaud = 9600.0;
constexpr int kSyncBits = 48, kMsgBits = 40, kFrameBits = 288;
constexpr uint64_t kSyncPattern = 0x555557125555ULL;   // the 48 sync bits, first bit sent is the top one

uint32_t bch(uint32_t data28);                          // the 12 check bits of a 28-bit message
bool bchOk(uint64_t voted40);                           // 40 bits (data << 12 | check) verify

// One frame, as bits (1 = higher frequency): the 48 sync bits, then message A and message B (data, 12 check bits), each three times (middle inverted).
// checkA / checkB: -1 writes the real BCH bits, anything else is written as given (to test corrupted frames).
std::vector<uint8_t> encodeFrame(uint32_t msgA, uint32_t msgB, int checkA = -1, int checkB = -1);

// Majority vote of a received message: 120 bits (copy, inverted copy, copy). Returns the 40 voted bits (packed MSB first in a uint64) and the number of
// bit positions where the copies disagreed.
uint64_t vote(const uint8_t* bits120, int& disagreements);

// FM test signal at fs (bit 1 = +devHz), carrier offsetHz from 0 Hz. invert flips the polarity.
std::vector<cf32> modulate(const std::vector<uint8_t>& bits, double fs, double offsetHz, double devHz, float amp, bool invert = false);

} // namespace dect2::edacs
