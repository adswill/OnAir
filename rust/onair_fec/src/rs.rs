// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Byte-wise Reed-Solomon codes over GF(256) with generator roots alpha^0 .. alpha^(R-1) (first consecutive root 0), shortened from n = 255:
//! one generic encoder / decoder instantiated for DVB-T RS(204,188) (R = 16), ATSC RS(207,187) (R = 20) and DAB RS(120,110) (R = 10).
//!
//! Encoding is a polynomial division by the generator, done one feedback byte at a time with a precomputed row `v * g` per byte value (the rows are
//! `const` data). Decoding: remainder of the received word modulo g (zero exactly when every syndrome is zero), syndromes S_i = rem(alpha^i),
//! Berlekamp-Massey, Chien search over the n positions, Forney (error evaluator Omega = S * C mod x^R), then a re-check of the corrected word.
//! The corrections are written into the word as they are found, so a failed decode can leave a partly corrected word; the DAB variant (`keep_input`)
//! works on a copy instead, like the C++ it replaces.
use crate::gf::GF;

/// Largest codeword (n = 255) and largest number of parity bytes handled by the fixed work buffers.
const MAX_N: usize = 255;

/// One Reed-Solomon code: `R` parity bytes. `rows[v][j]` = v * g_(R-1-j) where g(x) = x^R + g_(R-1) x^(R-1) + ... + g_0.
pub struct RsCode<const R: usize> {
    rows: [[u8; R]; 256],
}

impl<const R: usize> RsCode<R> {
    /// Builds the generator g(x) = prod_{i<R} (x + alpha^i) and the feedback rows at compile time.
    pub const fn new() -> RsCode<R> {
        // p[0..=R]: low power first, monic
        let mut p = [0u8; 33];
        p[0] = 1;
        let mut i = 0;
        while i < R {
            // multiply by (x + alpha^i): new[j + 1] ^= p[j], new[j] ^= p[j] * alpha^i
            let mut n = [0u8; 33];
            let mut j = 0;
            while j <= i {
                n[j + 1] ^= p[j];
                n[j] ^= GF.mul(p[j], GF.exp[i]);
                j += 1;
            }
            p = n;
            i += 1;
        }
        let mut rows = [[0u8; R]; 256];
        let mut v = 0;
        while v < 256 {
            let mut j = 0;
            while j < R {
                rows[v][j] = GF.mul(v as u8, p[R - 1 - j]);
                j += 1;
            }
            v += 1;
        }
        RsCode { rows }
    }

    /// Remainder of the word `w` (first byte = highest power) modulo g(x), as R bytes (highest power first). `w` is at most 255 bytes.
    fn remainder(&self, w: &[u8], rem: &mut [u8; R]) {
        let n = w.len();
        let mut b = [0u8; MAX_N + 32];
        b[..n].copy_from_slice(w);
        // the message part is consumed from the front; the remainder builds up in the R bytes behind it
        let k = n.saturating_sub(R);
        for i in 0..k {
            let fb = b[i] as usize;
            if fb == 0 {
                continue;
            }
            for (d, r) in b[i + 1..i + 1 + R].iter_mut().zip(self.rows[fb].iter()) {
                *d ^= *r;
            }
        }
        // a word shorter than R bytes (never passed by the wrappers) is padded with zeros in front
        for (j, r) in rem.iter_mut().enumerate() {
            let idx = k + j;
            *r = if idx < n { b[idx] } else { 0 };
        }
    }

    /// Systematic encoder: `out` (n = k + R bytes) gets `data` (k bytes) followed by the R parity bytes. Returns false when the lengths do not fit
    /// (`out.len() != data.len() + R`, or more than 255 bytes).
    pub fn encode(&self, data: &[u8], out: &mut [u8]) -> bool {
        if out.len() != data.len() + R || out.len() > MAX_N {
            return false;
        }
        let mut b = [0u8; MAX_N + 32];
        b[..data.len()].copy_from_slice(data);
        for i in 0..data.len() {
            let fb = b[i] as usize;
            if fb == 0 {
                continue;
            }
            for (d, r) in b[i + 1..i + 1 + R].iter_mut().zip(self.rows[fb].iter()) {
                *d ^= *r;
            }
        }
        out[..data.len()].copy_from_slice(data);
        out[data.len()..].copy_from_slice(&b[data.len()..data.len() + R]);
        true
    }

    /// True when the word has a non-zero remainder; `s` receives the syndromes S_i = rem(alpha^i).
    fn syndromes(&self, w: &[u8], s: &mut [u8; R]) -> bool {
        let mut rem = [0u8; R];
        self.remainder(w, &mut rem);
        if rem.iter().all(|&x| x == 0) {
            *s = [0; R];
            return false;
        }
        for (i, si) in s.iter_mut().enumerate() {
            let mut a = 0u8;
            for &r in rem.iter() {
                a = GF.mul(a, GF.exp[i]) ^ r;
            }
            *si = a;
        }
        true
    }

    /// Decodes `r` (the full word, n bytes) in place. Returns the number of corrected bytes (0 for a clean word), -1 when the word is uncorrectable
    /// (more than R/2 errors or a failed re-check), -2 when `r.len()` is outside R+1..=255. With `keep_input` the word is only changed on success.
    pub fn decode(&self, r: &mut [u8], keep_input: bool) -> i32 {
        let n = r.len();
        if n <= R || n > MAX_N {
            return -2;
        }
        if keep_input {
            let mut copy = [0u8; MAX_N];
            copy[..n].copy_from_slice(r);
            let v = self.decode_inplace(&mut copy[..n]);
            if v >= 0 {
                r.copy_from_slice(&copy[..n]);
            }
            v
        } else {
            self.decode_inplace(r)
        }
    }

    fn decode_inplace(&self, r: &mut [u8]) -> i32 {
        let n = r.len();
        let mut s = [0u8; R];
        if !self.syndromes(r, &mut s) {
            return 0;
        }
        // Berlekamp-Massey
        let mut c = [0u8; 33];
        let mut bb = [0u8; 33];
        c[0] = 1;
        bb[0] = 1;
        let mut l = 0usize;
        let mut m = 1usize;
        let mut b = 1u8;
        for i in 0..R {
            let mut d = s[i];
            for j in 1..=l {
                d ^= GF.mul(c[j], s[i - j]);
            }
            if d == 0 {
                m += 1;
                continue;
            }
            let t = c;
            let coef = GF.mul(d, GF.inv(b));
            for j in 0..=R {
                if j + m > R {
                    break;
                }
                c[j + m] ^= GF.mul(coef, bb[j]);
            }
            if 2 * l <= i {
                l = i + 1 - l;
                bb = t;
                b = d;
                m = 1;
            } else {
                m += 1;
            }
        }
        if l > R / 2 {
            return -1;
        }
        // Chien search: an error at power p (byte n-1-p) when C(alpha^-p) = 0
        let mut pos = [0usize; 16];
        let mut cnt = 0usize;
        for p in 0..n {
            let x = GF.exp[(255 - p) % 255];
            let mut v = 0u8;
            for i in (0..=l).rev() {
                v = GF.mul(v, x) ^ c[i];
            }
            if v == 0 {
                if cnt >= R / 2 {
                    return -1;
                }
                pos[cnt] = p;
                cnt += 1;
            }
        }
        if cnt != l {
            return -1;
        }
        // error evaluator Omega = S * C mod x^R
        let mut om = [0u8; 32];
        for i in 0..R {
            let mut a = 0u8;
            for j in 0..=i.min(l) {
                a ^= GF.mul(c[j], s[i - j]);
            }
            om[i] = a;
        }
        for &p in pos.iter().take(cnt) {
            let xp = GF.exp[p % 255];
            let xi = GF.exp[(255 - p) % 255];
            let mut num = 0u8;
            for i in (0..R).rev() {
                num = GF.mul(num, xi) ^ om[i];
            }
            // C'(x) keeps the odd terms: sum of C[i] x^(i-1) for odd i
            let mut den = 0u8;
            let mut i = 1;
            while i <= l {
                den ^= GF.mul(c[i], GF.exp[((255 - p) * (i - 1)) % 255]);
                i += 2;
            }
            if den == 0 {
                return -1;
            }
            r[n - 1 - p] ^= GF.mul(GF.mul(num, xp), GF.inv(den));
        }
        let mut s2 = [0u8; R];
        if self.syndromes(r, &mut s2) {
            return -1;
        }
        cnt as i32
    }
}

impl<const R: usize> Default for RsCode<R> {
    fn default() -> RsCode<R> {
        RsCode::new()
    }
}

/// DVB-T RS(204,188), t = 8 (EN 300 744 section 4.4: shortened from RS(255,239), field polynomial 0x11D, g = prod_{i=0}^{15} (x + alpha^i)).
pub static DVBT: RsCode<16> = RsCode::new();
/// ATSC RS(207,187), t = 10 (A/53 part 2 section 6.4.1).
pub static ATSC: RsCode<20> = RsCode::new();
/// DAB RS(120,110), t = 5 (EN 300 401 section 5.3.3, shortened from RS(255,245)).
pub static DAB: RsCode<10> = RsCode::new();
