// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Single DES (FIPS 46-3), ECB. Used by the GK-2A xRIT session layer, which encrypts every image segment with one 64-bit key
// per key number (KMA decryption keys, imported by the user). No keys are built in.
#pragma once
#include <cstddef>
#include <cstdint>

namespace dect2 {

struct Des {
    uint64_t sub[16];                      // the 16 round keys, 48 bits each
    void setKey(const uint8_t key[8]);     // parity bits are ignored
    uint64_t block(uint64_t in, bool decrypt) const;
    // ECB over n bytes (a multiple of 8; a short tail is left alone), in place
    void ecb(uint8_t* data, size_t n, bool decrypt) const;
};

} // namespace dect2
