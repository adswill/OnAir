// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Reed-Solomon over GF(64) (x^6 + x + 1), shortened from (63, ...), as used by the P25 header, link control and encryption sync (TIA-102.BAAA annex):
//! `sym[0]` is the first hexbit sent, the data first, then n - k parity symbols. Systematic encoder, Berlekamp-Massey decoder with Chien search and Forney
//! error values, and a final syndrome check. Block length at most 63.
use crate::gf::{div, mul, pw};

/// Longest supported block (the field size minus one).
pub const MAX_N: usize = 63;

/// Fills `sym[k..n]` with the parity of `sym[0..k]` (symbols masked to 6 bits). False for an unsupported (n, k) (nothing written).
pub fn encode(sym: &mut [u8], n: usize, k: usize) -> bool {
    if n > MAX_N || k > n || sym.len() < n {
        return false;
    }
    let np = n - k;
    let mut gp = [0u8; MAX_N + 1]; // descending powers, gp[0] = 1
    gp[0] = 1;
    for i in 1..=np {
        // multiply by (x + a^i)
        let a = pw(i as i32);
        for j in (1..=i).rev() {
            gp[j] ^= mul(gp[j - 1], a);
        }
    }
    let mut r = [0u8; MAX_N];
    for i in 0..k {
        r[i] = sym[i] & 63;
    }
    for i in 0..k {
        let c = r[i];
        if c != 0 {
            for j in 1..=np {
                r[i + j] ^= mul(c, gp[j]);
            }
        }
    }
    sym[k..n].copy_from_slice(&r[k..n]);
    true
}

/// Decodes and corrects `sym[0..n]` in place. Returns (true when the word is a codeword after correction, the number of errors corrected: 0 when decoding
/// stopped before the error values were applied). On failure the symbols may be partly modified, like the reference. Symbols are expected in 0..63.
pub fn decode(sym: &mut [u8], n: usize, k: usize) -> (bool, i32) {
    if n > MAX_N || k > n || sym.len() < n {
        return (false, 0);
    }
    let np = n - k;
    let mut s = [0u8; MAX_N];
    let mut any = false;
    for i in 1..=np {
        let mut v = 0u8;
        for t in 0..n {
            if sym[t] & 63 != 0 {
                v ^= mul(sym[t] & 63, pw(i as i32 * (n as i32 - 1 - t as i32)));
            }
        }
        s[i - 1] = v;
        if v != 0 {
            any = true;
        }
    }
    if !any {
        return (true, 0);
    }
    // Berlekamp-Massey
    let mut c = [0u8; MAX_N + 1];
    let mut b = [0u8; MAX_N + 1];
    c[0] = 1;
    b[0] = 1;
    let (mut l, mut m, mut bb) = (0usize, 1usize, 1u8);
    for r in 0..np {
        let mut d = s[r];
        for i in 1..=l {
            d ^= mul(c[i], s[r - i]);
        }
        if d == 0 {
            m += 1;
            continue;
        }
        let t = c;
        let coef = div(d, bb);
        let mut i = 0;
        while i + m <= np {
            c[i + m] ^= mul(coef, b[i]);
            i += 1;
        }
        if 2 * l <= r {
            l = r + 1 - l;
            b = t;
            bb = d;
            m = 1;
        } else {
            m += 1;
        }
    }
    if 2 * l > np {
        return (false, 0);
    }
    // Chien search over the n positions (power j = n - 1 - t)
    let mut pos = [0usize; MAX_N];
    let mut npos = 0;
    for j in 0..n {
        let mut v = 0u8;
        for i in 0..=l {
            v ^= mul(c[i], pw(-(j as i32) * i as i32));
        }
        if v == 0 {
            pos[npos] = j;
            npos += 1;
        }
    }
    if npos != l {
        return (false, 0);
    }
    // Forney (first root a^1): e = Omega(X^-1) / Lambda'(X^-1)
    let mut om = [0u8; MAX_N];
    for i in 0..np {
        for j in 0..=i.min(l) {
            om[i] ^= mul(c[j], s[i - j]);
        }
    }
    for &j in &pos[..npos] {
        let mut o = 0u8;
        let mut dl = 0u8;
        for i in 0..np {
            o ^= mul(om[i], pw(-(j as i32) * i as i32));
        }
        let mut i = 1;
        while i <= l {
            dl ^= mul(c[i], pw(-(j as i32) * (i as i32 - 1)));
            i += 2;
        }
        if dl == 0 {
            return (false, 0);
        }
        let e = div(o, dl);
        let at = n - 1 - j;
        sym[at] = (sym[at] ^ e) & 63;
    }
    // check
    for i in 1..=np {
        let mut v = 0u8;
        for t in 0..n {
            if sym[t] != 0 {
                v ^= mul(sym[t], pw(i as i32 * (n as i32 - 1 - t as i32)));
            }
        }
        if v != 0 {
            return (false, l as i32);
        }
    }
    (true, l as i32)
}
