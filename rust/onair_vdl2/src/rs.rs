// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Reed-Solomon (255,249) over GF(2^8) as VDL Mode 2 uses it: field polynomial 0x187, first root alpha^120, six roots. A burst's last block is shorter and carries
//! fewer parity octets (the others are erasures for the decoder). ICAO Annex 10 Vol III Part I section 6.3.2, Doc 9776.
use crate::bits::{RS_K, RS_N};

const FCR: i32 = 120;
const ROOTS: usize = 6;

/// Antilog and log tables of GF(2^8) with the field polynomial 0x187, built at compile time.
struct Gf {
    exp: [u8; 512],
    log: [u8; 256],
}

const fn build() -> Gf {
    let mut g = Gf { exp: [0; 512], log: [0; 256] };
    let mut x: u32 = 1;
    let mut i = 0;
    while i < 255 {
        g.exp[i] = x as u8;
        g.log[x as usize] = i as u8;
        x <<= 1;
        if x & 0x100 != 0 {
            x ^= 0x187;
        }
        i += 1;
    }
    let mut i = 255;
    while i < 512 {
        g.exp[i] = g.exp[i - 255];
        i += 1;
    }
    g
}

static GF: Gf = build();

fn mul(a: u8, b: u8) -> u8 {
    if a != 0 && b != 0 {
        GF.exp[GF.log[a as usize] as usize + GF.log[b as usize] as usize]
    } else {
        0
    }
}

fn div(a: u8, b: u8) -> u8 {
    if a != 0 {
        GF.exp[(GF.log[a as usize] as usize + 255 - GF.log[b as usize] as usize) % 255]
    } else {
        0
    }
}

/// alpha^e (e may be negative).
fn pw(e: i32) -> u8 {
    let mut e = e % 255;
    if e < 0 {
        e += 255;
    }
    GF.exp[e as usize]
}

/// The generator polynomial, highest power first (g[0] = 1).
fn gen_poly() -> [u8; ROOTS + 1] {
    let mut p = [0u8; ROOTS + 1];
    p[0] = 1;
    for deg in 0..ROOTS {
        let r = pw(FCR + deg as i32);
        let mut q = [0u8; ROOTS + 1];
        for k in 0..=deg {
            q[k] ^= p[k];
            q[k + 1] ^= mul(p[k], r);
        }
        p = q;
    }
    p
}

/// The six parity octets of 249 data octets.
pub fn encode(data: &[u8; RS_K]) -> [u8; ROOTS] {
    let g = gen_poly();
    let mut r = [0u8; ROOTS];
    for &d in data.iter() {
        let fb = d ^ r[0];
        for k in 0..ROOTS - 1 {
            r[k] = r[k + 1] ^ mul(fb, g[k + 1]);
        }
        r[ROOTS - 1] = mul(fb, g[ROOTS]);
    }
    r
}

/// Decodes a codeword in place; `n_parity` (0..6, clamped) parity octets are present, the rest are erased. Returns the octets corrected (erasures not
/// counted) or -1 when the block cannot be corrected.
pub fn decode(cw: &mut [u8; RS_N], n_parity: i32) -> i32 {
    let n_parity = n_parity.clamp(0, ROOTS as i32);
    let n = RS_N as i32;
    // syndromes: S_i = c(a^(fcr+i)), c[0] the highest power
    let mut s = [0u8; ROOTS];
    let mut zero = true;
    for (i, si) in s.iter_mut().enumerate() {
        let a = pw(FCR + i as i32);
        let mut v = 0u8;
        for &c in cw.iter() {
            v = mul(v, a) ^ c;
        }
        *si = v;
        zero &= v == 0;
    }
    let n_er = ROOTS as i32 - n_parity;
    if zero && n_er == 0 {
        return 0;
    }
    // erasure locator: positions RS_K + nParity .. N - 1, locator a^(N-1-j)
    let mut lam = [0u8; ROOTS + 1];
    lam[0] = 1;
    for e in 0..n_er {
        let j = RS_K as i32 + n_parity + e;
        let x = pw(n - 1 - j);
        for k in (1..=ROOTS).rev() {
            lam[k] ^= mul(x, lam[k - 1]);
        }
    }
    let mut b = lam;
    let mut l = n_er;
    for r in (n_er + 1)..=ROOTS as i32 {
        let mut d = 0u8;
        for i in 0..r as usize {
            d ^= mul(lam[i], s[r as usize - 1 - i]);
        }
        let mut xb = [0u8; ROOTS + 1];
        for k in (1..=ROOTS).rev() {
            xb[k] = b[k - 1];
        }
        if d == 0 {
            b = xb;
            continue;
        }
        let mut t = [0u8; ROOTS + 1];
        for k in 0..=ROOTS {
            t[k] = lam[k] ^ mul(d, xb[k]);
        }
        if 2 * l < r + n_er {
            l = r + n_er - l;
            for k in 0..=ROOTS {
                b[k] = div(lam[k], d);
            }
        } else {
            b = xb;
        }
        lam = t;
    }
    let mut deg = 0usize;
    for (k, &v) in lam.iter().enumerate() {
        if v != 0 {
            deg = k;
        }
    }
    if deg == 0 {
        return if zero { 0 } else { -1 };
    }
    // Chien search over the 255 positions
    let mut pos = [0usize; ROOTS];
    let mut nr = 0usize;
    for j in 0..RS_N {
        let e = n - 1 - j as i32; // locator exponent of position j
        let mut v = 0u8;
        for (k, &lk) in lam.iter().enumerate().take(deg + 1) {
            v ^= mul(lk, pw(-e * k as i32));
        }
        if v == 0 {
            if nr == ROOTS {
                return -1;
            }
            pos[nr] = j;
            nr += 1;
        }
    }
    if nr != deg {
        return -1;
    }
    // evaluator omega = S(x) lambda(x) mod x^roots
    let mut om = [0u8; ROOTS];
    for i in 0..ROOTS {
        for k in 0..=i.min(deg) {
            om[i] ^= mul(lam[k], s[i - k]);
        }
    }
    let mut fixed = 0i32;
    for &j in pos.iter().take(nr) {
        let e = n - 1 - j as i32;
        let xinv = pw(-e);
        let mut o = 0u8;
        let mut xp = 1u8;
        for &omi in om.iter() {
            o ^= mul(omi, xp);
            xp = mul(xp, xinv);
        }
        let mut dl = 0u8;
        let mut k = 1usize;
        while k <= deg {
            dl ^= mul(lam[k], pw(-e * (k as i32 - 1)));
            k += 2;
        }
        if dl == 0 {
            return -1;
        }
        let val = mul(div(o, dl), pw(e * (1 - FCR)));
        if val != 0 && (j as i32) < RS_K as i32 + n_parity {
            fixed += 1;
        }
        cw[j] ^= val;
    }
    if 2 * fixed + n_er > ROOTS as i32 {
        return -1;
    }
    fixed
}
