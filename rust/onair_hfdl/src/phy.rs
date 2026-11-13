// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The bit layers of an HFDL burst (ARINC 635 / ICAO Doc 9741 as dumphfdl implements them, read for reference): the eight rates, the known symbols, the
//! 15-bit m-sequence scrambler, the 40-row block interleaver, the rate 1/2 K = 7 convolutional code (generators 0x6D first, then 0x4F: checked on air, PR #175)
//! and its soft Viterbi decoder, and the FCS (CRC-16/X.25, sent low octet first).
use onair_common::crc::crc16_x25;

/// Symbols of the known part from A1 to the last training block.
pub const PRE_LEN: usize = 2 * 127 + 127 + 15 + 9 * 15;
const DATA_LEN: i32 = 30;

/// One of the eight rates (the M1 shift index).
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Rate {
    /// The index 0 .. 7.
    pub m1: i32,
    /// Bits per second (300, 600, 1200, 1800).
    pub bps: i32,
    /// Bits per data symbol.
    pub bits_per_sym: i32,
    /// Every coded bit is sent twice (300 bit/s).
    pub quarter: u8,
    /// Two slots (4.2 s interleaver).
    pub two_slots: u8,
    /// Data blocks of 30 symbols.
    pub blocks: i32,
    /// Interleaver column shift.
    pub col_shift: i32,
}

const fn r(m1: i32, bps: i32, b: i32, quarter: u8, two: u8, blocks: i32, sh: i32) -> Rate {
    Rate { m1, bps, bits_per_sym: b, quarter, two_slots: two, blocks, col_shift: sh }
}
static RATES: [Rate; 8] = [
    r(0, 300, 1, 1, 0, 72, 17),
    r(1, 600, 1, 0, 0, 72, 17),
    r(2, 1200, 2, 0, 0, 72, 17),
    r(3, 1800, 3, 0, 0, 72, 17),
    r(4, 300, 1, 1, 1, 168, 23),
    r(5, 600, 1, 0, 1, 168, 23),
    r(6, 1200, 2, 0, 1, 168, 23),
    r(7, 1800, 3, 0, 1, 168, 23),
];

/// The rate for `m1`, clamped into 0 .. 7.
pub fn rate(m1: i32) -> &'static Rate {
    &RATES[m1.clamp(0, 7) as usize]
}
/// Symbols from A1 to the last probe (the prekey not counted).
pub fn burst_syms(m1: i32) -> i32 {
    PRE_LEN as i32 + rate(m1).blocks * 45
}
/// Bits on the channel.
pub fn coded_bits(m1: i32) -> i32 {
    let r = rate(m1);
    r.blocks * DATA_LEN * r.bits_per_sym
}
/// Octets one burst carries.
pub fn payload_octets(m1: i32) -> i32 {
    let r = rate(m1);
    let info = coded_bits(m1) / if r.quarter != 0 { 4 } else { 2 };
    (info - 6) / 8
}

const A_OCT: [u8; 16] = [0x5B, 0xBC, 0x74, 0x57, 0x03, 0xD9, 0x89, 0x39, 0xF2, 0x08, 0xD5, 0x36, 0x94, 0x2C, 0x32, 0xFE];
const M_BASE: &[u8; 127] =
    b"0111011011110100010110010111110001000000110011011000111001110101110000100110000010101011010010010100111100100011010100001111111";
const M_SHIFT: [usize; 8] = [72, 82, 113, 123, 61, 103, 93, 9];
/// The probe / training symbols T, +-1.
pub const T: [i8; 15] = [1, 1, 1, -1, 1, 1, -1, -1, 1, -1, 1, -1, -1, -1, -1];

/// Bit `k` (0 .. 126, other values wrap through the octets) of the A sequence.
pub fn a_bit(k: i32) -> i32 {
    let k = k as u32 as usize;
    ((A_OCT[(k >> 3) & 15] >> (7 - (k & 7))) & 1) as i32
}
/// Bit `k` of the M sequence with cyclic shift `shift` (0 .. 7).
pub fn m_bit(shift: i32, k: i32) -> i32 {
    let s = M_SHIFT[(shift & 7) as usize];
    (M_BASE[(s + (k as u32 as usize % 127)) % 127] - b'0') as i32
}

/// The 531 known symbols from A1 to the last training block (+-1) for rate `m1` into `out` (needs `PRE_LEN`); false when `out` is too small.
pub fn preamble_ref(m1: i32, out: &mut [i8]) -> bool {
    if out.len() < PRE_LEN {
        return false;
    }
    let m = m1.clamp(0, 7);
    let mut i = 0;
    let mut put = |v: i8| {
        out[i] = v;
        i += 1;
    };
    for _ in 0..2 {
        for k in 0..127 {
            put(if a_bit(k) != 0 { -1 } else { 1 });
        }
    }
    for k in 0..127 {
        put(if m_bit(m, k) != 0 { -1 } else { 1 });
    }
    for k in 0..15 {
        put(if m_bit(m, k) != 0 { -1 } else { 1 });
    }
    for _ in 0..9 {
        for t in T {
            put(t);
        }
    }
    true
}

fn parity(v: u32) -> u32 {
    v.count_ones() & 1
}

/// The scrambler signs (+1 / -1) for `out.len()` data symbols: a 15-bit m-sequence (new bit = parity of bits 14 and 0 of the state, shifted in at bit 0, state
/// 0x4D4B) restarted every 120 data symbols; a 1 turns the symbol by 180 degrees.
pub fn scrambler_signs(out: &mut [i8]) {
    let mut st = 0x4D4Bu32;
    for (i, o) in out.iter_mut().enumerate() {
        if i % 120 == 0 {
            st = 0x4D4B;
        }
        let b = parity(st & 0x4001);
        st = ((st << 1) | b) & 0x7FFF;
        *o = if b != 0 { -1 } else { 1 };
    }
}

/// The interleaver for rate `m1`: `out[j]` is where the j-th decoder-order bit is sent, a permutation of 0 .. coded bits - 1 (channel bits are written down the
/// 40 rows, the column moving back by the shift after every bit and on by one after each pass over the rows; the decoder reads them with a row step of 9, the
/// column moving on after each pass). `out` needs room for twice the coded bits (the second half is scratch); returns the coded bit count, or `usize::MAX`
/// when `out` is too small.
pub fn interleave(m1: i32, out: &mut [i32]) -> usize {
    let n = coded_bits(m1) as usize;
    if out.len() < 2 * n {
        return usize::MAX;
    }
    let cols = n / 40;
    let sh = rate(m1).col_shift as usize;
    let (tab, cell) = out.split_at_mut(n);
    let cell = &mut cell[..n];
    cell.fill(0);
    let (mut row, mut col) = (0usize, 0usize);
    for i in 0..n {
        cell[row * cols + col] = i as i32;
        row += 1;
        if row == 40 {
            row = 0;
            col += 1;
        }
        col = (col + cols - sh % cols) % cols;
    }
    row = 0;
    col = 0;
    for t in tab.iter_mut() {
        *t = cell[row * cols + col];
        row = (row + 9) % 40;
        if row == 0 {
            col += 1;
        }
    }
    n
}

/// Rate 1/2 K = 7 convolutional encoder: two coded bits per input bit, the 0x6D output first, then 0x4F (dumphfdl's libfec V27POLYA 0x6d, V27POLYB 0x4f).
/// `out` needs `2 * bits.len()`; returns false when it is smaller.
pub fn conv_encode(bits: &[u8], out: &mut [u8]) -> bool {
    if out.len() < 2 * bits.len() {
        return false;
    }
    let mut sr = 0u32;
    for (i, b) in bits.iter().enumerate() {
        sr = ((sr << 1) | (*b as u32 & 1)) & 0x7F;
        out[2 * i] = parity(sr & 0x6D) as u8;
        out[2 * i + 1] = parity(sr & 0x4F) as u8;
    }
    true
}

const fn out_tab(poly: u32) -> [u8; 128] {
    let mut t = [0u8; 128];
    let mut r = 0;
    while r < 128 {
        t[r] = ((r as u32 & poly).count_ones() & 1) as u8;
        r += 1;
    }
    t
}
static OUT0: [u8; 128] = out_tab(0x6D);
static OUT1: [u8; 128] = out_tab(0x4F);

fn max_index(m: &[f32; 64]) -> (usize, f32) {
    let mut bi = 0;
    for i in 1..64 {
        if m[bi] < m[i] {
            bi = i;
        }
    }
    (bi, m[bi])
}

/// Soft Viterbi decoder for the code above. `soft` holds two values per input bit (positive = 0); decodes `n` bits into `out[..n]`. `surv` is scratch with one
/// `u64` per decoded bit (the survivor choices of the 64 states). Returns false when a buffer is too small.
pub fn viterbi(soft: &[f32], n: usize, out: &mut [u8], surv: &mut [u64]) -> bool {
    if soft.len() < n.saturating_mul(2) || out.len() < n || surv.len() < n {
        return false;
    }
    let mut m = [-1e30f32; 64];
    let mut nm = [0f32; 64];
    m[0] = 0.0;
    for t in 0..n {
        let s0 = soft[2 * t];
        let s1 = soft[2 * t + 1];
        let mut bits = 0u64;
        #[allow(clippy::needless_range_loop)]
        for ns in 0..64usize {
            let b = ns & 1;
            let mut best = -1e30f32;
            let mut bp = 0usize;
            for p in 0..2usize {
                let ps = (ns >> 1) | (p << 5);
                let reg = ((ps << 1) | b) & 0x7F;
                let bm = (if OUT0[reg] != 0 { -s0 } else { s0 }) + (if OUT1[reg] != 0 { -s1 } else { s1 });
                let v = m[ps] + bm;
                if v > best {
                    best = v;
                    bp = p;
                }
            }
            nm[ns] = best;
            bits |= (bp as u64) << ns;
        }
        surv[t] = bits;
        let (_, mx) = max_index(&nm);
        for s in 0..64 {
            m[s] = nm[s] - mx;
        }
    }
    let (mut s, _) = max_index(&m);
    for t in (0..n).rev() {
        out[t] = (s & 1) as u8;
        s = (s >> 1) | ((((surv[t] >> s) & 1) as usize) << 5);
    }
    true
}

/// The FCS of `p` (sent low octet first): CRC-16/X.25.
pub fn fcs(p: &[u8]) -> u16 {
    crc16_x25(p)
}
/// True when `p` ends with the FCS of everything before it (`p` = octets + 2 FCS octets).
pub fn fcs_ok(p: &[u8]) -> bool {
    if p.len() < 2 {
        return false;
    }
    let n = p.len() - 2;
    let f = fcs(&p[..n]);
    p[n] == (f & 0xFF) as u8 && p[n + 1] == (f >> 8) as u8
}
