// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The CRC-16 of DAB (ETSI EN 300 401 section 5.2.1, FIB / data group / packet / PAD): CCITT polynomial 0x1021, start 0xFFFF, result inverted.

/// Lookup table, built at compile time.
const TABLE: [u16; 256] = {
    let mut t = [0u16; 256];
    let mut i = 0;
    while i < 256 {
        let mut c = (i as u16) << 8;
        let mut k = 0;
        while k < 8 {
            c = if c & 0x8000 != 0 { (c << 1) ^ 0x1021 } else { c << 1 };
            k += 1;
        }
        t[i] = c;
        i += 1;
    }
    t
};

/// CRC-16 of `d`: a block that ends with its own CRC field (sent high byte first) checks when `crc16(block[..n-2]) == be16(block[n-2..])`.
pub fn crc16(d: &[u8]) -> u16 {
    let mut c = 0xFFFFu16;
    for &b in d {
        c = (c << 8) ^ TABLE[(((c >> 8) as u8) ^ b) as usize];
    }
    !c
}

/// Big-endian 16 bit value at the end of a block (0 when it is shorter than 2 bytes).
pub fn tail16(d: &[u8]) -> u16 {
    if d.len() < 2 {
        0
    } else {
        ((d[d.len() - 2] as u16) << 8) | d[d.len() - 1] as u16
    }
}
