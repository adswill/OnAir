// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The Mode S parity: CRC-24 and single-bit error correction (ICAO Annex 10 Vol. IV 3.1.2.3.3).

/// G(x) = x^24 + x^23 + ... + x^10 + x^3 + 1 without the x^24 term (Annex 10 Vol. IV 3.1.2.3.3.1).
pub const GENERATOR: u32 = 0xFFF409;

const fn make_table() -> [u32; 256] {
    let mut t = [0u32; 256];
    let mut b = 0;
    while b < 256 {
        let mut c = (b as u32) << 16;
        let mut i = 0;
        while i < 8 {
            c = if c & 0x80_0000 != 0 { (c << 1) ^ GENERATOR } else { c << 1 };
            i += 1;
        }
        t[b] = c & 0xFF_FFFF;
        b += 1;
    }
    t
}

static TABLE: [u32; 256] = make_table();

/// The division over the data bits (with 24 zero bits behind them): the parity a transmitter appends.
fn of_data(m: &[u8]) -> u32 {
    m.iter().fold(0u32, |c, &b| ((c << 8) ^ TABLE[(((c >> 16) ^ b as u32) & 0xFF) as usize]) & 0xFF_FFFF)
}

/// Remainder of the whole message (`nbits` multiple of 8, parity included): 0 for a good DF11 / DF17 / DF18 (DF11: the interrogator code, 0 - 127),
/// the aircraft address for the address / parity formats. 0 for fewer than 24 bits or a shorter `m` than `nbits` says.
pub fn crc24(m: &[u8], nbits: i32) -> u32 {
    let nb = nbits / 8;
    if nb < 3 || m.len() < nb as usize {
        return 0;
    }
    let nb = nb as usize;
    of_data(&m[..nb - 3]) ^ ((m[nb - 3] as u32) << 16 | (m[nb - 2] as u32) << 8 | m[nb - 1] as u32)
}

/// The 24 parity bits of a message whose first `nbits - 24` bits are filled in (the rest is ignored); XOR the address in for the address /
/// parity formats.
pub fn parity(m: &[u8], nbits: i32) -> u32 {
    let nb = nbits / 8;
    if nb < 3 || m.len() < nb as usize {
        return 0;
    }
    of_data(&m[..nb as usize - 3])
}

/// Message length of a downlink format in bits: 56, 112, or 0 when the format is not one the receiver searches for. DF 24 - 27 are the extended
/// length messages (ELM): bit 3 is a spare bit and 0, so DF 28 - 31 are not one.
pub fn df_length(df: i32) -> i32 {
    match df {
        0 | 4 | 5 | 11 => 56,
        16..=21 => 112,
        24..=27 => 112,
        _ => 0,
    }
}

/// The remainder produced by flipping bit `pos` (0 based from the first bit) of an `nbits` message.
pub fn single_bit_syndrome(nbits: i32, pos: i32) -> u32 {
    let mut m = [0u8; 14];
    if pos >= 0 && (pos >> 3) < 14 {
        m[(pos >> 3) as usize] = 0x80 >> (pos & 7);
    }
    crc24(&m, nbits)
}

const fn syndromes<const N: usize>() -> [u32; N] {
    let t = make_table();
    let mut s = [0u32; N];
    let mut p = 0;
    while p < N {
        // bit p flipped in a message of N bits: the data bytes before the parity and the parity bytes
        let mut m = [0u8; 14];
        m[p >> 3] = 0x80 >> (p & 7);
        let nb = N / 8;
        let mut c = 0u32;
        let mut i = 0;
        while i < nb - 3 {
            c = ((c << 8) ^ t[(((c >> 16) ^ m[i] as u32) & 0xFF) as usize]) & 0xFF_FFFF;
            i += 1;
        }
        s[p] = c ^ ((m[nb - 3] as u32) << 16 | (m[nb - 2] as u32) << 8 | m[nb - 1] as u32);
        p += 1;
    }
    s
}

static SYN56: [u32; 56] = syndromes::<56>();
static SYN112: [u32; 112] = syndromes::<112>();

/// Single-bit error correction: the position (0 based from the first bit) whose flip explains this remainder, or -1. Only 56 and 112 bit messages.
pub fn single_bit_position(nbits: i32, rem: u32) -> i32 {
    let t: &[u32] = if nbits == 56 { &SYN56 } else { &SYN112 };
    t.iter().position(|&s| s == rem).map_or(-1, |p| p as i32)
}
