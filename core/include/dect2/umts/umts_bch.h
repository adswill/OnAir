// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// UMTS broadcast channel: P-CCPCH (SF 256, code 1, 18 bits per slot outside the first 256 chips) -> two interleavers -> rate 1/2 convolutional decoding -> CRC16: one 246-bit transport block per 20 ms; and the
// BCCH content that names a cell: the master information block (MCC, MNC) and SIB 3 (cell identity), per TS 25.331 (unaligned PER).
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace umts {

constexpr int kBchInfo = 246, kBchBits = 270;

// encode one transport block (246 bits, one per byte) to the 540 channel bits of its two frames (frame 0, then frame 1)
std::vector<uint8_t> bchEncode(const uint8_t* tb246);
// soft channel bits (positive = 0) of the two frames, 540 values -> tb246; true when the CRC is right
bool bchDecode(const float* soft540, uint8_t* tb246);

// transport block content
std::vector<uint8_t> bchMakeMib(int mcc, int mnc, int mncDigits, int valueTag = 1, int sfnPrime = 0);       // 246 bits, a "complete SIB" block of the MIB
std::vector<uint8_t> bchMakeSib3(uint32_t cellIdentity, int sfnPrime = 0);

struct BchInfo {
    bool haveMib = false, haveSib3 = false;
    int mcc = -1, mnc = -1, mncDigits = 0;
    uint32_t cellIdentity = 0;
    int sfnPrime = -1;
};
class BchParser {
public:
    void feed(const uint8_t* tb246);   // handles complete SIBs and their segments
    const BchInfo& info() const { return info_; }
private:
    void sib(int type, const std::vector<uint8_t>& bits);
    std::map<int, std::vector<uint8_t>> seg_;
    std::map<int, int> segNext_, segCount_;
    BchInfo info_;
};

}  // namespace umts
}  // namespace dect2
