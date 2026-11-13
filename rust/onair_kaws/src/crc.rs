// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! CRC-16-CCITT (poly 0x1021, init 0xFFFF, "123456789" -> 0x29B1) and CRC-24 (OpenPGP: poly 0x864CFB, init 0xB704CE, "123456789" -> 0x21CF02), MSB first.
pub fn crc16(d: &[u8]) -> u16 {
    let mut c: u16 = 0xFFFF;
    for &b in d {
        c ^= (b as u16) << 8;
        for _ in 0..8 {
            c = if c & 0x8000 != 0 { (c << 1) ^ 0x1021 } else { c << 1 };
        }
    }
    c
}
pub fn crc24(d: &[u8]) -> u32 {
    let mut c: u32 = 0xB7_04CE;
    for &b in d {
        c ^= (b as u32) << 16;
        for _ in 0..8 {
            c <<= 1;
            if c & 0x100_0000 != 0 { c ^= 0x186_4CFB; }
        }
    }
    c & 0xFF_FFFF
}
/// CRC-16 over a bit string (one bit per byte, MSB first), zero padded to a byte boundary.
pub fn crc16_bits(bits: &[u8]) -> u16 {
    let mut bytes = alloc::vec![0u8; bits.len().div_ceil(8)];
    for (i, &b) in bits.iter().enumerate() {
        if b & 1 != 0 { bytes[i / 8] |= 0x80 >> (i % 8); }
    }
    crc16(&bytes)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn check_values() {
        assert_eq!(crc16(b"123456789"), 0x29B1);
        assert_eq!(crc24(b"123456789"), 0x21CF02);
    }
}
