// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode S / ADS-B message layer. See adsb_msg.h for the sources.
#include "dect2/adsb/adsb_msg.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "dect2/common/bits.h"
#include "dect2/common/str.h"

namespace dect2::adsb {

// Bit access with the standards' numbering and the hex text: simple helpers that stay C++ in every build (they take no length, so they cannot cross
// the Rust boundary safely, and the generator and the trackers call them in loops)
uint32_t getBits(const uint8_t* m, int first, int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; i++) {
        const int b = first - 1 + i;
        v = (v << 1) | ((m[b >> 3] >> (7 - (b & 7))) & 1u);
    }
    return v;
}

void setBits(uint8_t* m, int first, int count, uint32_t value) {
    for (int i = 0; i < count; i++) {
        const int b = first - 1 + i;
        const uint8_t mask = (uint8_t)(0x80 >> (b & 7));
        if ((value >> (count - 1 - i)) & 1u) m[b >> 3] |= mask; else m[b >> 3] &= (uint8_t)~mask;
    }
}

std::string toHex(const uint8_t* b, int nbits) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < nbits / 8; i++) { s += d[b[i] >> 4]; s += d[b[i] & 15]; }
    return s;
}


} // namespace dect2::adsb
