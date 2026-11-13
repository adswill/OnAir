// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! NXDN CRCs over bit arrays (one bit per byte), NXDN TS 1-A section 4: CRC-6, CRC-12 and CRC-16 with all-ones start values.

/// Bitwise CRC over `bits` (one bit per byte, bit 0 used): `w` bits wide, polynomial `poly`, start value `init`.
fn crc_gen(bits: &[u8], w: u32, poly: u32, init: u32) -> u32 {
    let mask = (1u32 << w) - 1;
    let mut c = init & mask;
    for &b in bits {
        let a = (b as u32 & 1) ^ (c >> (w - 1));
        c = (c << 1) & mask;
        if a != 0 {
            c ^= poly;
        }
    }
    c
}

/// CRC-6 (x^6 + x^5 + x^2 + x + 1), the SACCH check.
pub fn crc6(bits: &[u8]) -> u32 {
    crc_gen(bits, 6, 0x27, 0x3F)
}
/// CRC-12 (x^12 + x^11 + x^3 + x^2 + x + 1), the FACCH1 check.
pub fn crc12(bits: &[u8]) -> u32 {
    crc_gen(bits, 12, 0x80F, 0xFFF)
}
/// CRC-16 (x^16 + x^12 + x^5 + 1, CCITT, start 0xFFFF), the CAC check.
pub fn crc16(bits: &[u8]) -> u32 {
    crc_gen(bits, 16, 0x1021, 0xFFFF)
}
