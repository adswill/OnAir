// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! D-STAR header FEC (JARL D-STAR specification, radio header): the K=3 convolutional code (generators 7 and 5), the 24-column block interleaver over the 660
//! header bits, the scrambler x^7 + x^4 + 1 (all ones start), a soft Viterbi decoder, and the least-significant-bit-first byte / bit conversions.

/// Header length on air in bits (328 + 2 tail bits, rate 1/2).
pub const HEADER_BITS: usize = 660;
/// Header length in bytes (3 flags, 3 x 8 + 8 callsigns, 4 suffix, 2 CRC).
pub const HEADER_BYTES: usize = 41;
/// Longest message the Viterbi decoder supports (input bits).
pub const MAX_VITERBI: usize = 1024;

/// Bytes to bits, LSB first: `bits[8i + k]` is bit k of byte i.
pub fn bytes_to_bits(b: &[u8], bits: &mut [u8]) {
    for (i, &x) in b.iter().enumerate() {
        for k in 0..8 {
            if let Some(o) = bits.get_mut(i * 8 + k) {
                *o = (x >> k) & 1;
            }
        }
    }
}

/// Bits to bytes, LSB first (the inverse of [`bytes_to_bits`]); bits past the end of `bits` read as 0.
pub fn bits_to_bytes(bits: &[u8], b: &mut [u8]) {
    for (i, o) in b.iter_mut().enumerate() {
        let mut v = 0u8;
        for k in 0..8 {
            v |= (bits.get(i * 8 + k).copied().unwrap_or(0) & 1) << k;
        }
        *o = v;
    }
}

/// Convolutional encoder: `inp.len()` bits -> twice as many (`out` must hold them): out = b ^ d1 ^ d2 and b ^ d2.
pub fn conv_encode(inp: &[u8], out: &mut [u8]) {
    let (mut d1, mut d2) = (0u8, 0u8);
    for (i, &x) in inp.iter().enumerate() {
        let b = x & 1;
        if 2 * i + 1 < out.len() {
            out[2 * i] = b ^ d1 ^ d2;
            out[2 * i + 1] = b ^ d2;
        }
        d2 = d1;
        d1 = b;
    }
}

/// Soft Viterbi decoder: `n_out` bits from `2 * n_out` soft values (positive = 1), the tail bits bring the coder back to state 0. Returns the number of
/// channel bits that differ from the re-encoded result, or `None` when `n_out` is above [`MAX_VITERBI`] or a buffer is too short (nothing written).
pub fn viterbi(soft: &[f32], n_out: usize, out: &mut [u8]) -> Option<i32> {
    if n_out > MAX_VITERBI || soft.len() < 2 * n_out || out.len() < n_out {
        return None;
    }
    // state = (d1, d2) as d1 | d2 << 1; metric: correlation, larger is better
    let mut from = [0u8; MAX_VITERBI * 4];
    let mut m = [0.0f64, -1e30, -1e30, -1e30];
    for i in 0..n_out {
        let mut nm = [-1e30f64; 4];
        let mut nf = [0u8; 4];
        for s in 0..4usize {
            if m[s] < -1e29 {
                continue;
            }
            let d1 = s & 1;
            let d2 = s >> 1;
            for b in 0..2usize {
                let g1 = b ^ d1 ^ d2;
                let g2 = b ^ d2;
                let (a0, a1) = (soft[2 * i], soft[2 * i + 1]);
                let v = m[s] + f64::from(if g1 != 0 { a0 } else { -a0 }) + f64::from(if g2 != 0 { a1 } else { -a1 });
                let ns = b | (d1 << 1);
                if v > nm[ns] {
                    nm[ns] = v;
                    nf[ns] = s as u8;
                }
            }
        }
        for s in 0..4 {
            m[s] = nm[s];
            from[i * 4 + s] = nf[s];
        }
    }
    let mut s = 0usize; // the tail bits bring the coder back to state 0
    for i in (0..n_out).rev() {
        out[i] = (s & 1) as u8;
        s = from[i * 4 + s] as usize;
    }
    let mut re = [0u8; MAX_VITERBI * 2];
    conv_encode(&out[..n_out], &mut re[..2 * n_out]);
    let mut err = 0;
    for i in 0..2 * n_out {
        if (soft[i] > 0.0) != (re[i] != 0) {
            err += 1;
        }
    }
    Some(err)
}

/// Interleaver: input bit i goes to position `ILV[i]`.
const fn interleave_table() -> [u16; HEADER_BITS] {
    let mut t = [0u16; HEADER_BITS];
    let mut k = 0usize;
    let mut i = 0;
    while i < HEADER_BITS {
        t[i] = k as u16;
        k += 24;
        if k >= 672 {
            k -= 671;
        } else if k >= 660 {
            k -= 647;
        }
        i += 1;
    }
    t
}
static ILV: [u16; HEADER_BITS] = interleave_table();

/// Interleaves 660 bits.
pub fn interleave(inp: &[u8; HEADER_BITS], out: &mut [u8; HEADER_BITS]) {
    for i in 0..HEADER_BITS {
        out[ILV[i] as usize] = inp[i];
    }
}

/// De-interleaves 660 soft values.
pub fn deinterleave(inp: &[f32; HEADER_BITS], out: &mut [f32; HEADER_BITS]) {
    for i in 0..HEADER_BITS {
        out[i] = inp[ILV[i] as usize];
    }
}

/// XORs the scrambler sequence (x^7 + x^4 + 1, register starts all ones) into `bits`; applying it twice restores them.
pub fn scramble_bits(bits: &mut [u8]) {
    let mut s: u32 = 0x7F;
    for b in bits.iter_mut() {
        let o = ((s >> 3) ^ (s >> 6)) & 1;
        s = ((s << 1) | o) & 0x7F;
        *b ^= o as u8;
    }
}

/// Header on the air: 41 bytes -> 660 bits (convolutional code, interleaver, scrambler).
pub fn header_encode(hdr: &[u8; HEADER_BYTES], bits: &mut [u8; HEADER_BITS]) {
    let mut inp = [0u8; 330];
    bytes_to_bits(hdr, &mut inp);
    let mut c = [0u8; HEADER_BITS];
    conv_encode(&inp, &mut c);
    interleave(&c, bits);
    scramble_bits(bits);
}

/// Header decoder: 660 soft values (positive = 1) -> 41 bytes; returns the channel bit errors the Viterbi decoder corrected.
pub fn header_decode(soft: &[f32; HEADER_BITS], hdr: &mut [u8; HEADER_BYTES]) -> i32 {
    let mut sc = [0u8; HEADER_BITS];
    scramble_bits(&mut sc);
    let mut d = [0f32; HEADER_BITS];
    for i in 0..HEADER_BITS {
        d[i] = if sc[i] != 0 { -soft[i] } else { soft[i] };
    }
    let mut c = [0f32; HEADER_BITS];
    deinterleave(&d, &mut c);
    let mut out = [0u8; 330];
    let err = viterbi(&c, 330, &mut out).unwrap_or(0);
    bits_to_bytes(&out, hdr);
    err
}
