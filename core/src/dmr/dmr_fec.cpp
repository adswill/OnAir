// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DMR bit helpers (see dmr_fec.h). The error control codes of ETSI TS 102 361-1 annex B are Rust: dmr_fec_rs.cpp over rust/onair_dmr.
#include "dect2/dmr/dmr_fec.h"
#include "dect2/common/bits.h"

namespace dect2 {
namespace dmr {

// ---------------------------------------------------------------------------------------------------- bit helpers

void putBits(Bits& b, uint64_t v, int n) {
    for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1));
}

uint64_t getBits(const Bits& b, size_t pos, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | (pos + i < b.size() ? (b[pos + i] & 1) : 0);
    return v;
}

void bytesToBits(const uint8_t* d, size_t n, Bits& out) {
    for (size_t i = 0; i < n; i++) putBits(out, d[i], 8);
}

void bitsToBytes(const Bits& b, size_t pos, size_t nbits, uint8_t* out) {
    for (size_t i = 0; i < nbits / 8; i++) out[i] = (uint8_t)getBits(b, pos + i * 8, 8);
}

int popcount32(uint32_t v) { return dect2::bits::popcount(v); }

} // namespace dmr
} // namespace dect2
