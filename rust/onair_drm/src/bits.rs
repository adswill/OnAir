// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Bit level helpers of DRM: the CRCs of ES 201 980 annex D over unpacked bits (one bit per byte, only bit 0 used) and a bit reader. Reads past the end give 0.

/// Bit-serial MSB-first CRC over unpacked bits (`width` 1..32), result XORed with `xorout`.
pub fn crc_bits(b: &[u8], width: u32, poly: u32, init: u32, xorout: u32) -> u32 {
    let mask = if width >= 32 { u32::MAX } else { (1u32 << width) - 1 };
    let mut r = init & mask;
    for &x in b {
        let fb = ((r >> (width - 1)) & 1) ^ (x & 1) as u32;
        r = (r << 1) & mask;
        if fb != 0 {
            r ^= poly;
        }
    }
    (r ^ xorout) & mask
}

/// CRC-8 of the FAC (G8 = x^8 + x^4 + x^3 + x^2 + 1, all ones at the start, complemented).
pub fn crc8(b: &[u8]) -> u32 {
    crc_bits(b, 8, 0x1D, 0xFF, 0xFF)
}

/// CRC-16 of the SDC (G16 = x^16 + x^12 + x^5 + 1, all ones at the start, complemented).
pub fn crc16(b: &[u8]) -> u32 {
    crc_bits(b, 16, 0x1021, 0xFFFF, 0xFFFF)
}

/// Reader over one-bit-per-byte data.
pub struct BitReader<'a> {
    p: &'a [u8],
    /// Position in bits.
    pub n: usize,
}

impl<'a> BitReader<'a> {
    /// A reader at bit 0.
    pub fn new(p: &'a [u8]) -> Self {
        BitReader { p, n: 0 }
    }
    /// The next `bits` bits, MSB first.
    pub fn get(&mut self, bits: u32) -> u32 {
        let mut v = 0u32;
        for _ in 0..bits {
            v = (v << 1) | (self.p.get(self.n).copied().unwrap_or(0) & 1) as u32;
            self.n += 1;
        }
        v
    }
}
