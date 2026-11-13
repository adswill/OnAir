// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The ACARS block check sequence (ARINC 618 section 3.2.3.4): CRC-16 of the "KERMIT" kind (reflected polynomial 0x8408,
//! start 0, no final xor), over every byte from the mode character to the suffix and then the two check bytes (low byte first): 0 for a good block.

const fn make_table() -> [u16; 256] {
    let mut t = [0u16; 256];
    let mut i = 0;
    while i < 256 {
        let mut c = i as u16;
        let mut k = 0;
        while k < 8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0x8408 } else { c >> 1 };
            k += 1;
        }
        t[i] = c;
        i += 1;
    }
    t
}

/// Number of byte positions the single-bit syndrome table covers (the longest block plus check bytes).
pub const MAX_SYN: usize = 270;

const fn make_syndromes() -> [[u16; 8]; MAX_SYN] {
    let t = make_table();
    let mut s = [[0u16; 8]; MAX_SYN];
    let mut b = 0;
    while b < 8 {
        s[0][b] = t[1 << b];
        b += 1;
    }
    let mut e = 1;
    while e < MAX_SYN {
        let mut b = 0;
        while b < 8 {
            // a zero byte after the error advances the residue one step
            s[e][b] = (s[e - 1][b] >> 8) ^ t[(s[e - 1][b] & 0xff) as usize];
            b += 1;
        }
        e += 1;
    }
    s
}

static TABLE: [u16; 256] = make_table();
static SYN: [[u16; 8]; MAX_SYN] = make_syndromes();

/// One byte into the running check sequence.
pub fn update(crc: u16, b: u8) -> u16 {
    (crc >> 8) ^ TABLE[((crc ^ b as u16) & 0xff) as usize]
}

/// The check sequence of `data` continuing from `crc`.
pub fn crc(data: &[u8], crc: u16) -> u16 {
    data.iter().fold(crc, |c, &b| update(c, b))
}

/// Odd parity over all 8 bits (ARINC 618: every character carries an odd-parity bit).
pub fn parity_ok(c: u8) -> bool {
    c.count_ones() & 1 != 0
}

/// Syndrome of a single flipped bit: the residue it leaves. `pos_from_end` 0 is the last check byte, 1 the first, 2 the last text byte...
/// 0 when the position or bit is out of range.
pub fn syndrome(pos_from_end: i32, bit: i32) -> u16 {
    if !(0..MAX_SYN as i32).contains(&pos_from_end) || !(0..8).contains(&bit) {
        return 0;
    }
    SYN[pos_from_end as usize][bit as usize]
}

/// Direct table access for the repair code (`e` < [`MAX_SYN`], `b` < 8).
pub(crate) fn syn(e: usize, b: usize) -> u16 {
    SYN[e][b]
}
