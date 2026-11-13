// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Line codes shared by the packet decoders: NRZI (HDLC/AX.25/AIS) and Manchester (IEEE 802.3 convention
// for the FLARM chips). Operate on unpacked bit vectors (one bit per byte).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace linecode {

// NRZI encode as used by HDLC (ITU-T X.25 / AX.25): a 0 bit toggles the line level, a 1 keeps it; startLevel is the level before bit 0.
inline std::vector<uint8_t> nrziEncode(const std::vector<uint8_t>& data, int startLevel) {
    std::vector<uint8_t> out(data.size());
    int lv = startLevel & 1;
    for (size_t i = 0; i < data.size(); i++) {
        if (!data[i]) lv ^= 1;
        out[i] = (uint8_t)lv;
    }
    return out;
}
// NRZI decode (inverse of the above): a bit is 1 when the line level is unchanged from the previous sample (prevLevel before sample 0).
inline std::vector<uint8_t> nrziDecode(const std::vector<uint8_t>& line, int prevLevel) {
    std::vector<uint8_t> out(line.size());
    int p = prevLevel & 1;
    for (size_t i = 0; i < line.size(); i++) {
        out[i] = (line[i] == p) ? 1 : 0;
        p = line[i];
    }
    return out;
}
// Manchester encode bytes MSB-first into chips: 1 -> 01, 0 -> 10 (two chips per bit).
inline std::vector<uint8_t> manchesterEncode(const uint8_t* bytes, size_t n) {
    std::vector<uint8_t> c;
    c.reserve(n * 16);
    for (size_t i = 0; i < n; i++)
        for (int k = 7; k >= 0; k--) { const int b = (bytes[i] >> k) & 1; c.push_back(b ? 0 : 1); c.push_back(b ? 1 : 0); }
    return c;
}

} // namespace linecode
} // namespace dect2
