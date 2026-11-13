// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! MPEG-2 section CRC-32 (ISO/IEC 13818-1 Annex A): polynomial 0x04C11DB7, start 0xFFFFFFFF, no reflection, no final XOR. A section that includes its
//! own CRC_32 field gives 0.

/// Lookup table, built at compile time.
const TABLE: [u32; 256] = {
    let mut t = [0u32; 256];
    let mut i = 0;
    while i < 256 {
        let mut c = (i as u32) << 24;
        let mut k = 0;
        while k < 8 {
            c = if c & 0x8000_0000 != 0 { (c << 1) ^ 0x04C1_1DB7 } else { c << 1 };
            k += 1;
        }
        t[i] = c;
        i += 1;
    }
    t
};

/// CRC-32/MPEG-2 of `p`.
pub fn mpeg_crc32(p: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for &b in p {
        crc = (crc << 8) ^ TABLE[((crc >> 24) ^ b as u32) as usize & 255];
    }
    crc
}
