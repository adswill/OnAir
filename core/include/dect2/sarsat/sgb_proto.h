// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// COSPAS-SARSAT second-generation beacons (SGB), message layer and spreading codes. Clean-room from the public specification C/S T.018 (Issue 1 Rev 4):
// 250 bit message = 202 information bits + 48 bit BCH(250,202) (corrects up to 6 bits), bits numbered from 1 as in the specification (bit n at index n-1).
// Odd bits go through the I channel, even bits through the Q channel; each channel bit is 256 chips of a PRN sequence (inverted for a 1), 38 400 chips/s.
// The rotating field (bits 155-202) is only identified (bits 155-158), its content is not decoded.
#pragma once
#include "dect2/sarsat/sarsat_proto.h"
#include <cstdint>
#include <vector>

namespace dect2::sarsat {

constexpr int kSgbInfoBits = 202, kSgbMsgBits = 250, kSgbChips = 38400, kSgbPreambleBits = 50;
constexpr double kSgbChipRate = 38400.0;

// The 38 400 chips of one PRN segment (Table 2.2): I or Q, normal or self-test mode. Chip value 0 = +1, 1 = -1.
std::vector<uint8_t> sgbPrn(bool q, bool selfTest);

// The 48 bit BCH check of the first 202 bits (as in annex B.1), and generator polynomial g(X) as a 49 bit number
uint64_t sgbBch(const std::vector<uint8_t>& info202);
uint64_t sgbGenerator();

// Checks and repairs (up to 6 bit errors) the 250 bits in place; returns the number of repaired bits, or -1 when the word is not repairable.
int sgbCorrect(std::vector<uint8_t>& bits250);

struct SgbSpec {
    int tac = 230, serial = 573, mid = 201;
    bool homing = false, rls = false, test = false;
    bool hasPos = true;
    double lat = 0, lon = 0;
    int vesselType = 0;          // bits 91-93
    uint64_t vesselId = 0;       // bits 94-137 (44 bits)
    int beaconType = 1;          // bits 138-140: 0 ELT, 1 EPIRB, 2 PLB, 3 ELT(DT)
    bool cancel = false;         // spare bits 141-154 all zero instead of all one
    int rotId = 0;               // bits 155-158
    uint64_t rotData = 0;        // bits 159-202 (44 bits)
};
std::vector<uint8_t> sgbEncode(const SgbSpec& s);   // 250 bits

// Fills `out` from 250 corrected bits (no check here besides the fixed fields: spare bits must be all ones or all zeros, beacon type not spare).
bool sgbDecode(const std::vector<uint8_t>& bits250, Beacon& out);
// 23 hex ID from the 202 information bits (section 3.6)
std::string sgbHexId(const std::vector<uint8_t>& bits);

}  // namespace dect2::sarsat
