// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! BCH(31,21) with an overall even parity bit: the 32-bit code word of POCSAG (ITU-R M.584-2, annex) and of FLEX (public descriptions).
//! A word has 21 data bits (first transmitted bit in bit 20 of the data), 10 check bits and one parity bit (bit 0). Up to two wrong bits are corrected.

/// Generator polynomial x^10 + x^9 + x^8 + x^6 + x^5 + x^3 + 1.
const GEN: u32 = 0x769;

/// Parity (1 when the number of set bits is odd) of a 32-bit value.
fn parity32(mut v: u32) -> u32 {
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    v & 1
}

/// Remainder of a 31-bit polynomial divided by the generator (usable in constants).
const fn remainder31(mut c: u32) -> u32 {
    let mut i = 30;
    while i >= 10 {
        if (c >> i) & 1 != 0 {
            c ^= GEN << (i - 10);
        }
        i -= 1;
    }
    c & 0x3FF
}

/// Syndrome to the positions (0 to 30 from the least significant bit) of one or two wrong bits; 255 = none.
struct SynTable {
    p1: [u8; 1024],
    p2: [u8; 1024],
}

/// Builds the syndrome table at compile time: same fill order as the C++ reference (single errors first, then pairs, first writer wins).
const fn build_table() -> SynTable {
    let mut t = SynTable { p1: [255; 1024], p2: [255; 1024] };
    let mut s = [0u32; 31];
    let mut i = 0;
    while i < 31 {
        s[i] = remainder31(1u32 << i);
        i += 1;
    }
    i = 0;
    while i < 31 {
        t.p1[s[i] as usize] = i as u8;
        i += 1;
    }
    i = 0;
    while i < 31 {
        let mut j = i + 1;
        while j < 31 {
            let x = (s[i] ^ s[j]) as usize;
            if t.p1[x] == 255 && t.p2[x] == 255 {
                t.p1[x] = i as u8;
                t.p2[x] = j as u8;
            }
            j += 1;
        }
        i += 1;
    }
    t
}

static SYN: SynTable = build_table();

/// Result of `correct_word`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fixed {
    /// True when the word was valid or could be repaired.
    pub ok: bool,
    /// Bits that were wrong (the parity bit counts).
    pub bits: i32,
    /// The 21 data bits.
    pub data: u32,
}

/// The 21 data bits (first transmitted bit in bit 20) to the 32-bit word with check bits and parity.
pub fn encode_word(data21: u32) -> u32 {
    let mut c = (data21 & 0x1F_FFFF) << 10;
    c |= remainder31(c);
    (c << 1) | parity32(c)
}

/// Checks a received 32-bit word and repairs up to two wrong bits (the parity bit counts as one).
pub fn correct_word(w: u32) -> Fixed {
    let bad = Fixed { ok: false, bits: 0, data: 0 };
    let mut c = w >> 1;
    let s = remainder31(c) as usize;
    let mut n = 0;
    if s != 0 {
        if SYN.p1[s] == 255 {
            return bad;
        }
        c ^= 1u32 << SYN.p1[s];
        n += 1;
        if SYN.p2[s] != 255 {
            c ^= 1u32 << SYN.p2[s];
            n += 1;
        }
    }
    let pbad = i32::from((w & 1) != parity32(c));
    if n + pbad > 2 {
        return bad;
    }
    Fixed { ok: true, bits: n + pbad, data: c >> 10 }
}

/// Reverses the order of the low 21 bits (FLEX sends its data words with the first bit in bit 0).
pub fn rev21(v: u32) -> u32 {
    let mut r = 0;
    for i in 0..21 {
        r |= ((v >> i) & 1) << (20 - i);
    }
    r
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn idle_and_sync_words() {
        // POCSAG idle code word 0x7A89C197 is data 0x0F5138 (ITU-R M.584-2)
        assert_eq!(encode_word(0x7A89C197 >> 11), 0x7A89C197);
        assert_eq!(encode_word(0x7CD215D8 >> 11), 0x7CD215D8);
    }
    #[test]
    fn corrects_two_errors() {
        let w = encode_word(0x12345);
        let f = correct_word(w ^ 0x0000_0802);
        assert!(f.ok && f.data == 0x12345 && f.bits == 2);
        assert!(!correct_word(w ^ 0b1110_0000).ok);
        assert_eq!(correct_word(w).bits, 0);
    }
    #[test]
    fn rev() {
        assert_eq!(rev21(1), 1 << 20);
        assert_eq!(rev21(rev21(0x1ABCDE)), 0x1ABCDE);
    }
}
