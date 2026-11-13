// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! P25 network identifier (NID, TIA-102.BAAA 7.x): NAC (12 bits) and DUID (4 bits) protected by the BCH (63,16,23) code plus an overall parity bit, 64 bits.
//! Decoding is a nearest codeword search over all 65536 messages (the code is linear, so each codeword is two table lookups).

/// BCH (63,16) generator polynomial 6331 1413 6723 5453 (octal), degree 47.
const GEN: u64 = 0o6331141367235453;

/// Codeword (bit 63 sent first: 16 data bits, 47 parity bits, overall parity) of a 16-bit message.
const fn codeword(m: u64) -> u64 {
    let mut r = m << 47;
    let mut i = 62;
    while i >= 47 {
        if (r >> i) & 1 != 0 {
            r ^= GEN << (i - 47);
        }
        i -= 1;
    }
    let c63 = (m << 47) | r;
    (c63 << 1) | (c63.count_ones() & 1) as u64
}

/// Codewords of the messages 0..255 (the low byte) and of the messages `i << 8` (the high byte): the code is linear, so a codeword is their XOR.
const fn table(shift: u32) -> [u64; 256] {
    let mut t = [0u64; 256];
    let mut i = 0;
    while i < 256 {
        t[i] = codeword((i as u64) << shift);
        i += 1;
    }
    t
}
static LOW: [u64; 256] = table(0);
static HIGH: [u64; 256] = table(8);

/// The 64 NID bits (one per byte, first sent first) of a NAC and DUID.
pub fn encode(nac: i32, duid: i32, bits: &mut [u8; 64]) {
    let c = codeword((((nac & 0xFFF) << 4) | (duid & 0xF)) as u64);
    for (i, b) in bits.iter_mut().enumerate() {
        *b = ((c >> (63 - i)) & 1) as u8;
    }
}

/// Decoded NID.
pub struct Nid {
    /// Network access code.
    pub nac: i32,
    /// Data unit id.
    pub duid: i32,
    /// Bit errors found: the distance to the nearest codeword (also when too far to accept).
    pub errs: i32,
    /// True when within 10 bit errors.
    pub ok: bool,
}

/// Decodes 64 bits: the nearest codeword (parity bit ignored) and its distance; accepted up to 10 errors.
pub fn decode(bits: &[u8; 64]) -> Nid {
    let mut x: u64 = 0;
    for &b in bits.iter() {
        x = (x << 1) | u64::from(b & 1);
    }
    let mut best = 99;
    let mut bm = 0usize;
    for m in 0..65536usize {
        let d = (((HIGH[m >> 8] ^ LOW[m & 255]) ^ x) >> 1).count_ones() as i32;
        if d < best {
            best = d;
            bm = m;
            if d == 0 {
                break;
            }
        }
    }
    if best > 10 {
        return Nid { nac: 0, duid: 0, errs: best, ok: false };
    }
    Nid { nac: (bm >> 4) as i32, duid: (bm & 0xF) as i32, errs: best, ok: true }
}
