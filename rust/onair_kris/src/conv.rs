// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! K = 7 convolutional code, rate 1/2 (generators 0o171, 0o133) and rate 1/4 (0o171, 0o133, 0o165, 0o117), and a soft-decision Viterbi decoder.
//! Register: reg = (input << 6) | state, state = the previous six inputs (newest in bit 5), next state = reg >> 1; output i = parity(reg & g_i).
//! Terminated blocks append six zero tail bits (coded too). LLR convention: positive = bit 0 more likely.
use alloc::vec;

pub const GENS: [u32; 4] = [0o171, 0o133, 0o165, 0o117];
pub const TAIL: usize = 6;

/// Coded bits per input bit: 2 for kind 0, 4 for kind 1.
pub fn n_out(kind: i32) -> Option<usize> {
    match kind {
        0 => Some(2),
        1 => Some(4),
        _ => None,
    }
}

/// Number of coded bits for `nbits` input bits.
pub fn coded_len(kind: i32, nbits: usize, terminate: bool) -> Option<usize> {
    Some((nbits + if terminate { TAIL } else { 0 }) * n_out(kind)?)
}

fn parity(x: u32) -> u8 {
    (x.count_ones() & 1) as u8
}

/// Encodes into `out` (at least `coded_len` bytes, one bit per byte); returns the number of bits written.
pub fn encode_into(kind: i32, bits: &[u8], terminate: bool, out: &mut [u8]) -> Option<usize> {
    let no = n_out(kind)?;
    let total = coded_len(kind, bits.len(), terminate)?;
    if out.len() < total {
        return None;
    }
    let mut state = 0u32;
    let mut w = 0;
    let steps = bits.len() + if terminate { TAIL } else { 0 };
    for i in 0..steps {
        let b = if i < bits.len() { (bits[i] & 1) as u32 } else { 0 };
        let reg = (b << 6) | state;
        for g in GENS.iter().take(no) {
            out[w] = parity(reg & g);
            w += 1;
        }
        state = reg >> 1;
    }
    Some(total)
}

/// Soft Viterbi. `llr` holds `nsym` groups of 2 (kind 0) or 4 (kind 1) values. Terminated: the path must end in state 0 and the six tail bits are dropped.
/// Unterminated: the best final metric wins. Writes the decoded bits to `out`; returns their number.
pub fn decode_into(kind: i32, llr: &[f32], nsym: usize, terminated: bool, out: &mut [u8]) -> Option<usize> {
    let no = n_out(kind)?;
    if llr.len() < nsym.checked_mul(no)? {
        return None;
    }
    let nbits = if terminated { nsym.checked_sub(TAIL)? } else { nsym };
    if out.len() < nbits {
        return None;
    }
    let mut pat = [0usize; 128];
    for (reg, p) in pat.iter_mut().enumerate() {
        for (i, g) in GENS.iter().take(no).enumerate() {
            *p |= (parity(reg as u32 & g) as usize) << i;
        }
    }
    const NEG: f32 = -1.0e6;
    let mut pm = [NEG; 64];
    pm[0] = 0.0;
    let mut surv = vec![0u64; nsym];
    for t in 0..nsym {
        let l = &llr[t * no..t * no + no];
        let mut bm = [0f32; 16];
        for (p, m) in bm.iter_mut().enumerate().take(1 << no) {
            let mut s = 0.0;
            for (i, &v) in l.iter().enumerate() {
                let v = if v.is_nan() { 0.0 } else { v };
                s += if (p >> i) & 1 == 1 { -v } else { v };
            }
            *m = s;
        }
        let mut npm = [0f32; 64];
        let mut dec = 0u64;
        for ns in 0..64usize {
            let s0 = (ns & 31) << 1;
            let reg0 = ((ns >> 5) << 6) | s0;
            let m0 = pm[s0] + bm[pat[reg0]];
            let m1 = pm[s0 | 1] + bm[pat[reg0 | 1]];
            if m1 > m0 {
                npm[ns] = m1;
                dec |= 1u64 << ns;
            } else {
                npm[ns] = m0;
            }
        }
        let best = npm.iter().cloned().fold(NEG * 4.0, |a, b| if b > a { b } else { a });
        for (d, s) in pm.iter_mut().zip(npm.iter()) {
            *d = *s - best;
        }
        surv[t] = dec;
    }
    let mut state = 0usize;
    if !terminated {
        let mut bm = pm[0];
        for (s, &m) in pm.iter().enumerate() {
            if m > bm {
                bm = m;
                state = s;
            }
        }
    }
    for t in (0..nsym).rev() {
        if t < nbits {
            out[t] = (state >> 5) as u8;
        }
        state = ((state & 31) << 1) | ((surv[t] >> state) & 1) as usize;
    }
    Some(nbits)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::xorshift::XorShift64;

    fn bits(seed: u64, n: usize) -> alloc::vec::Vec<u8> {
        let mut r = XorShift64::new(seed);
        (0..n).map(|_| (r.next() >> 33) as u8 & 1).collect()
    }

    #[test]
    fn first_symbols() {
        let mut out = [0u8; 32];
        assert_eq!(encode_into(0, &[1, 0, 0], false, &mut out), Some(6));
        // input 1: newest tap (bit 6) is set in every generator -> 11; then the 1 sits at register bit 5 -> g bit5
        assert_eq!(&out[..2], &[1, 1]);
        assert_eq!(&out[2..4], &[((0o171 >> 5) & 1) as u8, ((0o133 >> 5) & 1) as u8]);
        assert_eq!(encode_into(1, &[1], false, &mut out), Some(4));
        assert_eq!(&out[..4], &[1, 1, 1, 1]);
        assert_eq!(encode_into(2, &[1], false, &mut out), None);
        assert_eq!(encode_into(0, &[1; 20], true, &mut out), None); // buffer too small
        assert_eq!(coded_len(0, 100, true), Some(212));
        assert_eq!(coded_len(1, 100, false), Some(400));
    }

    fn llr_from(coded: &[u8], mag: f32) -> alloc::vec::Vec<f32> {
        coded.iter().map(|&b| if b == 0 { mag } else { -mag }).collect()
    }

    #[test]
    fn roundtrip_clean() {
        for kind in 0..2 {
            for &term in &[true, false] {
                let info = bits(3 + kind as u64, 300);
                let mut coded = vec![0u8; coded_len(kind, 300, term).unwrap()];
                encode_into(kind, &info, term, &mut coded).unwrap();
                let llr = llr_from(&coded, 2.0);
                let nsym = coded.len() / n_out(kind).unwrap();
                let mut out = vec![0u8; 300];
                assert_eq!(decode_into(kind, &llr, nsym, term, &mut out), Some(300));
                assert_eq!(out, info, "kind {kind} term {term}");
            }
        }
    }

    #[test]
    fn corrects_errors() {
        for kind in 0..2 {
            let info = bits(11, 400);
            let mut coded = vec![0u8; coded_len(kind, 400, true).unwrap()];
            encode_into(kind, &info, true, &mut coded).unwrap();
            let mut llr = llr_from(&coded, 2.0);
            let mut i = 7;
            while i < llr.len() {
                llr[i] = -llr[i]; // one hard error every 40 coded bits (rate 1/2) / 40 (rate 1/4)
                i += 40;
            }
            let nsym = coded.len() / n_out(kind).unwrap();
            let mut out = vec![0u8; 400];
            assert_eq!(decode_into(kind, &llr, nsym, true, &mut out), Some(400));
            assert_eq!(out, info, "kind {kind}");
        }
    }

    #[test]
    fn bad_sizes() {
        let mut out = [0u8; 4];
        assert_eq!(decode_into(0, &[0.0; 4], 2, true, &mut out), None); // nsym < tail
        assert_eq!(decode_into(0, &[0.0; 4], 3, false, &mut out), None); // llr too short
        assert_eq!(decode_into(5, &[0.0; 4], 2, false, &mut out), None);
    }
}
