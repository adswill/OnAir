// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Binary primitive shortened BCH codes of DVB-T2 (EN 302 755 section 6.1.1): normal frames use GF(2^16) (polynomial 0x1002D), short frames GF(2^14)
//! (0x402B), error correction t = 1..12, generator = product of the first t minimal polynomials of `bch_tables`.
//!
//! Bits are one per byte (0 or 1), the first bit has the highest power. Encoding is the remainder of message * x^deg modulo the generator; decoding
//! takes the remainder of the received word (a byte-at-a-time table does the division), turns it into syndromes S_j = r(alpha^j), runs
//! Berlekamp-Massey, a Chien search over all received positions and flips the bits found. The field tables are big (2^16 entries), so they live in a
//! caller owned `BchCtx` (the C++ side allocates `size_of::<BchCtx>()` bytes, 8-aligned, and calls `init` once per code); nothing here allocates.
use crate::bch_tables::{NORMAL, SHORT};

/// Words of the shift register: deg is at most 16 * 12 = 192 bits.
const W: usize = 3;
/// Largest t.
pub const MAX_T: usize = 12;

/// One DVB-T2 BCH code with its tables.
pub struct BchCtx {
    m: u32,
    t: u32,
    deg: u32,
    /// Field order minus one (2^m - 1).
    n_field: u32,
    /// alpha^i for i in 0..2^m - 1.
    exp: [u16; 65535],
    /// Log table (index 0 unused).
    log: [u16; 65536],
    /// Generator polynomial, `gen[i]` = coefficient of x^i, i < deg (x^deg implicit).
    gen: [u8; 192],
    /// The generator as register words (bit i = coefficient of x^i) and the mask of the top word.
    gw: [u64; W],
    mask: u64,
    /// Byte-wise remainder table: `tab[v]` = (v(x) * x^deg) mod g(x) for every byte v that leaves the register.
    tab: [[u64; W]; 256],
}

impl BchCtx {
    /// Size in bytes of the context (for the caller's allocation).
    pub const fn size() -> usize {
        core::mem::size_of::<BchCtx>()
    }

    /// Field product through the log tables.
    #[inline]
    fn fmul(&self, a: u32, b: u32) -> u32 {
        if a == 0 || b == 0 {
            return 0;
        }
        let mut i = self.log[a as usize] as u32 + self.log[b as usize] as u32;
        if i >= self.n_field {
            i -= self.n_field;
        }
        self.exp[i as usize] as u32
    }

    /// Fills the context for `t` (1..=12) errors on a short (16200 bit) or normal (64800 bit) frame. Returns false for another t.
    pub fn init(&mut self, short_frame: bool, t: u32) -> bool {
        if t == 0 || t as usize > MAX_T {
            return false;
        }
        let m: u32 = if short_frame { 14 } else { 16 };
        let prim: u32 = if short_frame { 0x402B } else { 0x1002D };
        self.m = m;
        self.t = t;
        self.deg = m * t;
        self.n_field = (1u32 << m) - 1;
        let n = self.n_field as usize;
        self.log = [0; 65536];
        self.exp = [0; 65535];
        let mut x: u32 = 1;
        for i in 0..n {
            self.exp[i] = x as u16;
            self.log[x as usize] = i as u16;
            x <<= 1;
            if x & (1u32 << m) != 0 {
                x ^= prim;
            }
        }
        // generator = product of the first t minimal polynomials (carry-less polynomial product over GF(2))
        let mut g = [0u8; 200];
        g[0] = 1;
        let mut glen = 1usize;
        for k in 0..t as usize {
            let plen = (m + 1) as usize;
            let mut out = [0u8; 200];
            for i in 0..glen {
                if g[i] != 0 {
                    for j in 0..plen {
                        let pj = if short_frame { SHORT[k][j] } else { NORMAL[k][j] };
                        if pj != 0 {
                            out[i + j] ^= 1;
                        }
                    }
                }
            }
            glen += plen - 1;
            g = out;
        }
        self.gen = [0; 192];
        self.gen.copy_from_slice(&g[..192]);
        self.gw = self.gen_words();
        self.mask = self.last_mask();
        // byte table: tab[v] = (v(x) * x^deg) mod g(x) for the 8 bits v that leave the register
        for v in 0..256usize {
            let mut r = [0u64; W];
            for b in (0..8).rev() {
                self.step_bit(&mut r, ((v >> b) & 1) as u64);
            }
            for _ in 0..self.deg {
                self.step_bit(&mut r, 0);
            }
            self.tab[v] = r;
        }
        true
    }

    fn gen_words(&self) -> [u64; W] {
        let mut g = [0u64; W];
        for i in 0..self.deg as usize {
            if self.gen[i] != 0 {
                g[i >> 6] |= 1u64 << (i & 63);
            }
        }
        g
    }

    fn last_mask(&self) -> u64 {
        if self.deg & 63 != 0 {
            (1u64 << (self.deg & 63)) - 1
        } else {
            !0u64
        }
    }

    /// One shift of the division register by one input bit.
    fn step_bit(&self, r: &mut [u64; W], bit: u64) {
        let deg = self.deg as usize;
        let w_used = (deg + 63) / 64;
        let top = (r[(deg - 1) >> 6] >> ((deg - 1) & 63)) & 1;
        for w in (1..w_used).rev() {
            r[w] = (r[w] << 1) | (r[w - 1] >> 63);
        }
        r[0] = (r[0] << 1) | bit;
        r[w_used - 1] &= self.mask;
        if top != 0 {
            for (x, g) in r.iter_mut().zip(self.gw.iter()).take(w_used) {
                *x ^= *g;
            }
        }
    }

    /// Shifts one byte (8 input bits, first bit highest) into the register through the byte table.
    #[inline]
    fn step_byte(&self, r: &mut [u64; W], byte: u64) {
        let deg = self.deg as usize;
        let w_used = (deg + 63) / 64;
        // the 8 highest register bits (deg-1 .. deg-8)
        let hb = deg - 8;
        let (hw, ho) = (hb >> 6, hb & 63);
        let mut t = r[hw] >> ho;
        if ho > 56 && hw + 1 < w_used {
            t |= r[hw + 1] << (64 - ho);
        }
        t &= 0xFF;
        for w in (1..w_used).rev() {
            r[w] = (r[w] << 8) | (r[w - 1] >> 56);
        }
        r[0] = (r[0] << 8) | byte;
        r[w_used - 1] &= self.mask;
        let tt = &self.tab[t as usize];
        for (x, t) in r.iter_mut().zip(tt.iter()).take(w_used) {
            *x ^= *t;
        }
    }

    /// Remainder of (`bits`, then `zeros` zero bits) modulo g(x) as a register: bit i of the result is the coefficient of x^i. Dividing is the same
    /// whatever the grouping into bytes, so the message goes in bytes, its last (up to 7) bits one by one, and the zeros again in bytes.
    fn remainder(&self, bits: &[u8], zeros: usize) -> [u64; W] {
        let mut r = [0u64; W];
        let nb8 = bits.len() & !7;
        for chunk in bits[..nb8].chunks_exact(8) {
            let byte = chunk.iter().fold(0u64, |a, &b| (a << 1) | (b & 1) as u64);
            self.step_byte(&mut r, byte);
        }
        for &b in &bits[nb8..] {
            self.step_bit(&mut r, (b & 1) as u64);
        }
        for _ in 0..zeros / 8 {
            self.step_byte(&mut r, 0);
        }
        for _ in 0..zeros % 8 {
            self.step_bit(&mut r, 0);
        }
        r
    }

    /// Number of parity bits (m * t).
    pub fn parity_bits(&self) -> usize {
        self.deg as usize
    }

    /// Systematic encoder: `bits[..k]` is the message, the parity goes to `bits[k..k + deg]`. False when the buffer is too short.
    pub fn encode(&self, bits: &mut [u8], k: usize) -> bool {
        let deg = self.deg as usize;
        match k.checked_add(deg) {
            Some(end) if end <= bits.len() => {}
            _ => return false,
        }
        let rem = self.remainder(&bits[..k], deg);
        for i in 0..deg {
            bits[k + i] = ((rem[(deg - 1 - i) >> 6] >> ((deg - 1 - i) & 63)) & 1) as u8;
        }
        true
    }

    /// Corrects up to t bit errors in place. Returns the number of corrected bits, -1 when uncorrectable.
    pub fn decode(&self, bits: &mut [u8]) -> i32 {
        let n = bits.len();
        let deg = self.deg as usize;
        let t = self.t as usize;
        let rem = self.remainder(bits, 0);
        if rem.iter().all(|&w| w == 0) {
            return 0;
        }
        let nf = self.n_field as u64;
        let bit = |i: usize| (rem[i >> 6] >> (i & 63)) & 1 != 0;
        // syndromes S_1 .. S_2t: S_j = sum over set bits i of the remainder of alpha^(i j)
        let mut s = [0u32; 2 * MAX_T + 1];
        for (j, sj) in s.iter_mut().enumerate().skip(1).take(2 * t) {
            // alpha^(i j): the exponent grows by j per bit position
            let mut a = 0u32;
            let mut e = 0u32;
            for i in 0..deg {
                if bit(i) {
                    a ^= self.exp[e as usize] as u32;
                }
                e += j as u32;
                if e >= self.n_field {
                    e -= self.n_field;
                }
            }
            *sj = a;
        }
        // Berlekamp-Massey
        let csz = 2 * t + 2;
        let mut c = [0u32; 2 * MAX_T + 2];
        let mut bb = [0u32; 2 * MAX_T + 2];
        c[0] = 1;
        bb[0] = 1;
        let mut l = 0usize;
        let mut mm = 1usize;
        let mut b = 1u32;
        for nn in 0..2 * t {
            let mut d = s[nn + 1];
            for i in 1..=l {
                d ^= self.fmul(c[i], s[nn + 1 - i]);
            }
            if d == 0 {
                mm += 1;
                continue;
            }
            let tmp = c;
            let inv_b = self.exp[((nf - self.log[b as usize] as u64) % nf) as usize] as u32;
            let coef = self.fmul(d, inv_b);
            for i in 0..csz {
                if i + mm >= csz {
                    break;
                }
                if bb[i] != 0 {
                    c[i + mm] ^= self.fmul(coef, bb[i]);
                }
            }
            if 2 * l <= nn {
                l = nn + 1 - l;
                bb = tmp;
                b = d;
                mm = 1;
            } else {
                mm += 1;
            }
        }
        if l > t {
            return -1;
        }
        // Chien search: an error at power p (bit n-1-p) when C(alpha^-p) = 0. Term i of C(alpha^-p) is alpha^(log C_i - p i): its exponent drops by i
        // per position, so no division is needed. Blocks of positions are summed term by term (an independent chain of table lookups per term).
        let mut pos = [0usize; MAX_T];
        let mut cnt = 0usize;
        let nfu = self.n_field;
        // terms: (exponent at the block start, step per position = n_field - i) of the non-zero coefficients
        let mut ex = [0u32; MAX_T + 1];
        let mut st = [0u32; MAX_T + 1];
        let mut nt = 0usize;
        for (i, &ci) in c.iter().enumerate().take(l + 1) {
            if ci != 0 {
                ex[nt] = self.log[ci as usize] as u32;
                st[nt] = (nfu - i as u32) % nfu;
                nt += 1;
            }
        }
        const BLK: usize = 256;
        let mut p0 = 0usize;
        while p0 < n {
            let len = BLK.min(n - p0);
            let mut acc = [0u16; BLK];
            for k in 0..nt {
                let mut e = ex[k];
                let step = st[k];
                for a in acc.iter_mut().take(len) {
                    *a ^= self.exp[e as usize];
                    e += step;
                    if e >= nfu {
                        e -= nfu;
                    }
                }
                ex[k] = e;
            }
            for (q, &a) in acc.iter().enumerate().take(len) {
                if a == 0 {
                    if cnt < MAX_T {
                        pos[cnt] = p0 + q;
                    }
                    cnt += 1;
                }
            }
            p0 += len;
        }
        if cnt != l {
            return -1;
        }
        for &p in pos.iter().take(cnt) {
            bits[n - 1 - p] ^= 1;
        }
        l as i32
    }
}
