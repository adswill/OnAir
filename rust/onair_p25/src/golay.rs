// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Short codes of the P25 voice and header frames: Hamming (10,6,3) of the LDU hexbits, Golay (24,12,8) (the extended (23,12) code with generator
//! x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1) and its shortened (18,6,8) form of the header. Decoders search all codewords (4096 / 64).

const GOLAY_GEN: u32 = 0xC75;
const HAM_PAR: [u32; 6] = [0xE, 0xD, 0xB, 0x7, 0x3, 0xC];

/// Golay (24,12) codeword of 12 data bits (data first: bit 23 is sent first, parity bit last).
const fn golay24(d12: u32) -> u32 {
    let mut r = (d12 & 0xFFF) << 11;
    let mut i = 22;
    while i >= 11 {
        if (r >> i) & 1 != 0 {
            r ^= GOLAY_GEN << (i - 11);
        }
        i -= 1;
    }
    let c23 = ((d12 & 0xFFF) << 11) | r;
    (c23 << 1) | (c23.count_ones() & 1)
}
const fn golay_table() -> [u32; 4096] {
    let mut t = [0u32; 4096];
    let mut d = 0;
    while d < 4096 {
        t[d] = golay24(d as u32);
        d += 1;
    }
    t
}
static GOLAY: [u32; 4096] = golay_table();

/// Hamming (10,6,3) codeword of a hexbit: the hexbit in the top six bits (bit 9 sent first), four check bits.
pub fn hamming10_encode(hex: i32) -> u16 {
    let mut par = 0u32;
    for i in 0..6 {
        if (hex >> (5 - i)) & 1 != 0 {
            par ^= HAM_PAR[i];
        }
    }
    ((((hex & 63) as u32) << 4) | par) as u16
}

/// Nearest hexbit and its distance (the first of the minimum): one error is corrected.
pub fn hamming10_decode(cw: u16) -> (i32, i32) {
    let mut best = 99;
    let mut bh = 0;
    for h in 0..64 {
        let d = (u32::from(hamming10_encode(h)) ^ (u32::from(cw) & 0x3FF)).count_ones() as i32;
        if d < best {
            best = d;
            bh = h;
        }
    }
    (bh, best)
}

/// Golay (24,12,8) codeword of 12 data bits.
pub fn golay24_encode(d12: i32) -> u32 {
    golay24(d12 as u32)
}

/// Nearest data word and its distance (stops at an exact match); the word is -1 when the distance exceeds three.
pub fn golay24_decode(cw: u32) -> (i32, i32) {
    let mut best = 99;
    let mut bd = 0;
    for d in 0..4096usize {
        let e = (GOLAY[d] ^ (cw & 0xFF_FFFF)).count_ones() as i32;
        if e < best {
            best = e;
            bd = d as i32;
            if e == 0 {
                break;
            }
        }
    }
    (if best <= 3 { bd } else { -1 }, best)
}

/// Shortened Golay (18,6,8) codeword of a hexbit.
pub fn golay18_encode(hex: i32) -> u32 {
    golay24((hex & 63) as u32) & 0x3FFFF
}

/// Nearest hexbit and its distance; the hexbit is -1 when the distance exceeds three.
pub fn golay18_decode(cw: u32) -> (i32, i32) {
    let mut best = 99;
    let mut bh = 0;
    for h in 0..64 {
        let e = (golay18_encode(h) ^ (cw & 0x3FFFF)).count_ones() as i32;
        if e < best {
            best = e;
            bh = h;
        }
    }
    (if best <= 3 { bh } else { -1 }, best)
}
