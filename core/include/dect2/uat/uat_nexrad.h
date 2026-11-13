// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FIS-B NEXRAD weather radar (products 63 regional, 64 CONUS): the block layout of RTCA DO-358B as read by Oliver Jowett's dump978 (extract_nexrad.c, GPL-2+).
// A block covers 48' of longitude (96' above 60 degrees) by 4' of latitude and holds 32 x 4 bins. Pure parsing, tested on simulated blocks.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {

struct UatNexradBlock {
    uint16_t productId = 63;
    uint32_t blockNum = 0;
    double latNorth = 0, lonWest = 0;     // degrees; longitude is 0..360 east as the block numbering gives it
    double latSize = 0, lonSize = 0;      // degrees of the whole block
    std::array<uint8_t, 128> bins{};      // intensity 0..7, west to east, north row first
};

// Decodes one APDU payload of product 63/64 into blocks (one RLE block, or the blocks of an empty-block bitmap). False when it does not parse.
bool uatNexradDecode(uint16_t productId, const uint8_t* d, size_t n, std::vector<UatNexradBlock>& out);
size_t uatNexradEncodeRle(const UatNexradBlock& b, int scale, bool south, std::vector<uint8_t>& out);   // test signal

} // namespace dect2
