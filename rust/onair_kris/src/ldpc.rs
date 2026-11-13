// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The Kris LDPC family: systematic quasi-cyclic IRA codes with a dual-diagonal parity part.
//!
//! n = 1920, 2304 or 7680; rates 1/4, 1/3, 1/2, 2/3, 3/4, 5/6, 9/10 (index 0..6). The parity-check matrix is H = [A | P] on a base graph of
//! mb x nb blocks, each block a Z x Z circulant (cyclic shift of the identity) or empty. P is block bidiagonal (identity on the diagonal and the
//! sub-diagonal), so encoding is back substitution: p0 = s0, p(i) = s(i) + p(i-1), where s(i) is the sum of the shifted information blocks of block row i.
//!
//! Lifting size: Z = 48 for n = 1920 and 2304, 160 for n = 7680 when it divides both n and k; otherwise the largest divisor of gcd(n, k) below that
//! (1920 at 1/3, 2/3 and 5/6 gets Z = 40, 7680 at 9/10 gets 128). n = 2304 at 9/10 has no integer k (2073.6): no such code.
//!
//! The A part comes from a fixed-seed xorshift64: the first nb/4-ish columns have weight 7, the rest weight 3 (capped by the number of block rows;
//! a column that cannot be placed without a 4-cycle drops to a lower weight), rows picked by lowest degree first, shifts chosen so that no 4-cycle
//! exists in the lifted graph (for every pair of block rows the shift differences over their common columns are distinct modulo Z).
//!
//! Decoder: layered normalised min-sum (alpha 0.8), f32 LLRs, positive = bit 0 more likely. Inner loops run over the Z lifted checks of a block
//! row, which are contiguous and branch-free (vectorisable).
use crate::xorshift::XorShift64;
use alloc::boxed::Box;
use alloc::vec;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicPtr, Ordering};

pub const BLOCK_LENGTHS: [usize; 3] = [1920, 2304, 7680];
/// (numerator, denominator) of rate index 0..6.
pub const RATES: [(usize, usize); 7] = [(1, 4), (1, 3), (1, 2), (2, 3), (3, 4), (5, 6), (9, 10)];
pub const ALPHA: f32 = 0.8;
const BIG: f32 = 1.0e4;

/// Information length of code (n, rate index), or None for an unsupported block length / rate or a non-integer k.
pub fn info_len(n: usize, rate: usize) -> Option<usize> {
    if !BLOCK_LENGTHS.contains(&n) || rate >= RATES.len() {
        return None;
    }
    let (a, b) = RATES[rate];
    if (n * a) % b != 0 {
        return None;
    }
    Some(n * a / b)
}

fn gcd(a: usize, b: usize) -> usize {
    if b == 0 {
        a
    } else {
        gcd(b, a % b)
    }
}

/// Lifting size for (n, k).
pub fn lifting(n: usize, k: usize) -> usize {
    let pref = if n == 7680 { 160 } else { 48 };
    let g = gcd(n, k);
    let mut d = pref;
    while d > 1 {
        if g % d == 0 {
            return d;
        }
        d -= 1;
    }
    1
}

pub struct Code {
    pub n: usize,
    pub k: usize,
    pub z: usize,
    pub nb: usize,
    pub kb: usize,
    pub mb: usize,
    /// Base matrix mb x nb: circulant shift, or -1 for an empty block (parity part: 0 on the diagonal and sub-diagonal).
    pub base: Vec<i32>,
    row_ptr: Vec<usize>,  // entries of block row r: row_ptr[r]..row_ptr[r+1] (information blocks first, then parity)
    info_end: Vec<usize>, // end of the information entries of block row r
    cols: Vec<u32>,
    shifts: Vec<u32>,
    max_deg: usize,
}

fn conflict(used: &[bool], mb: usize, z: usize, r1: usize, s1: usize, r2: usize, s2: usize) -> bool {
    if r1 < r2 {
        used[(r1 * mb + r2) * z + (s1 + z - s2) % z]
    } else {
        used[(r2 * mb + r1) * z + (s2 + z - s1) % z]
    }
}

fn mark(used: &mut [bool], mb: usize, z: usize, r1: usize, s1: usize, r2: usize, s2: usize) {
    if r1 < r2 {
        used[(r1 * mb + r2) * z + (s1 + z - s2) % z] = true;
    } else {
        used[(r2 * mb + r1) * z + (s2 + z - s1) % z] = true;
    }
}

impl Code {
    fn build(n: usize, rate: usize) -> Option<Code> {
        let k = info_len(n, rate)?;
        let z = lifting(n, k);
        let (nb, kb) = (n / z, k / z);
        let mb = nb - kb;
        if mb < 2 {
            return None;
        }
        let mut rng = XorShift64::new(0x9E37_79B9_7F4A_7C15 ^ ((n as u64) << 20) ^ ((rate as u64 + 1) << 8) ^ 0x4B52_4953);
        for _ in 0..8 {
            rng.next();
        }
        let mut used = vec![false; mb * mb * z];
        for j in 0..mb - 1 {
            used[(j * mb + j + 1) * z] = true; // the two parity blocks of a column share shift 0
        }
        let mut base = vec![-1i32; mb * nb];
        let mut deg = vec![0u32; mb];
        let heavy = core::cmp::max(1, kb / 4);
        for c in 0..kb {
            let mut w = core::cmp::min(if c < heavy { 7 } else { 3 }, mb);
            loop {
                let tries = if w > 1 { 400 } else { 1 };
                let mut placed: Option<(Vec<usize>, Vec<usize>)> = None;
                for _ in 0..tries {
                    let mut keyed: Vec<(u64, usize)> = (0..mb).map(|r| (deg[r] as u64 * 5 + rng.below(6) as u64, r)).collect();
                    keyed.sort();
                    let rows: Vec<usize> = keyed[..w].iter().map(|x| x.1).collect();
                    let mut sh: Vec<usize> = Vec::with_capacity(w);
                    let mut ok = true;
                    for (i, &r) in rows.iter().enumerate() {
                        let allowed: Vec<usize> =
                            (0..z).filter(|&s| (0..i).all(|j| !conflict(&used, mb, z, rows[j], sh[j], r, s))).collect();
                        if allowed.is_empty() {
                            ok = false;
                            break;
                        }
                        sh.push(allowed[rng.below(allowed.len() as u32) as usize]);
                    }
                    if ok {
                        placed = Some((rows, sh));
                        break;
                    }
                }
                if let Some((rows, sh)) = placed {
                    for i in 0..rows.len() {
                        for j in 0..i {
                            mark(&mut used, mb, z, rows[j], sh[j], rows[i], sh[i]);
                        }
                        base[rows[i] * nb + c] = sh[i] as i32;
                        deg[rows[i]] += 1;
                    }
                    break;
                }
                w -= 1; // w = 1 always places
            }
        }
        for r in 0..mb {
            base[r * nb + kb + r] = 0;
            if r + 1 < mb {
                base[(r + 1) * nb + kb + r] = 0;
            }
        }
        let (mut row_ptr, mut info_end, mut cols, mut shifts) = (vec![0usize], Vec::new(), Vec::new(), Vec::new());
        let mut max_deg = 0;
        for r in 0..mb {
            for c in 0..kb {
                let s = base[r * nb + c];
                if s >= 0 {
                    cols.push(c as u32);
                    shifts.push(s as u32);
                }
            }
            info_end.push(cols.len());
            for c in kb..nb {
                let s = base[r * nb + c];
                if s >= 0 {
                    cols.push(c as u32);
                    shifts.push(s as u32);
                }
            }
            max_deg = core::cmp::max(max_deg, cols.len() - row_ptr[r]);
            row_ptr.push(cols.len());
        }
        Some(Code { n, k, z, nb, kb, mb, base, row_ptr, info_end, cols, shifts, max_deg })
    }

    /// Systematic encode: `info` is k bits (one per byte), `cw` receives n bits (information first, then parity).
    pub fn encode(&self, info: &[u8], cw: &mut [u8]) -> bool {
        if info.len() != self.k || cw.len() != self.n {
            return false;
        }
        let (z, k) = (self.z, self.k);
        for i in 0..k {
            cw[i] = info[i] & 1;
        }
        let mut acc = vec![0u8; z];
        for r in 0..self.mb {
            acc.iter_mut().for_each(|a| *a = 0);
            for e in self.row_ptr[r]..self.info_end[r] {
                let (b, s) = (self.cols[e] as usize * z, self.shifts[e] as usize);
                for zz in 0..z - s {
                    acc[zz] ^= cw[b + s + zz];
                }
                for zz in z - s..z {
                    acc[zz] ^= cw[b + zz + s - z];
                }
            }
            if r > 0 {
                let prev = k + (r - 1) * z;
                for zz in 0..z {
                    acc[zz] ^= cw[prev + zz];
                }
            }
            cw[k + r * z..k + (r + 1) * z].copy_from_slice(&acc);
        }
        true
    }

    /// True when every parity check holds for the n hard bits.
    pub fn check(&self, bits: &[u8]) -> bool {
        if bits.len() != self.n {
            return false;
        }
        let z = self.z;
        let mut acc = vec![0u8; z];
        for r in 0..self.mb {
            acc.iter_mut().for_each(|a| *a = 0);
            for e in self.row_ptr[r]..self.row_ptr[r + 1] {
                let (b, s) = (self.cols[e] as usize * z, self.shifts[e] as usize);
                for zz in 0..z - s {
                    acc[zz] ^= bits[b + s + zz] & 1;
                }
                for zz in z - s..z {
                    acc[zz] ^= bits[b + zz + s - z] & 1;
                }
            }
            if acc.iter().any(|&a| a != 0) {
                return false;
            }
        }
        true
    }

    fn syndrome_ok(&self, l: &[f32], hard: &mut [u8]) -> bool {
        for (h, v) in hard.iter_mut().zip(l) {
            *h = (v.to_bits() >> 31) as u8;
        }
        self.check(hard)
    }

    /// Layered normalised min-sum. `llr` has n values; `info` receives the k hard decisions (also on failure). Returns the iterations used (0 = the
    /// input already satisfied every check), or -1 when the checks never passed.
    pub fn decode(&self, llr: &[f32], max_iter: i32, info: &mut [u8]) -> i32 {
        if llr.len() != self.n || info.len() != self.k {
            return -1;
        }
        let z = self.z;
        let mut l: Vec<f32> = llr.iter().map(|&v| if v.is_nan() { 0.0 } else { v.clamp(-30.0, 30.0) }).collect();
        let mut hard = vec![0u8; self.n];
        let finish = |l: &[f32], info: &mut [u8]| {
            for (i, o) in info.iter_mut().enumerate() {
                *o = (l[i].to_bits() >> 31) as u8;
            }
        };
        if self.syndrome_ok(&l, &mut hard) {
            finish(&l, info);
            return 0;
        }
        let mut rmsg = vec![0f32; self.cols.len() * z];
        let mut q = vec![0f32; self.max_deg * z];
        let mut min1 = vec![0f32; z];
        let mut min2 = vec![0f32; z];
        let mut idx = vec![0u32; z];
        let mut sgn = vec![0u32; z];
        for it in 1..=max_iter {
            for r in 0..self.mb {
                let (a, b) = (self.row_ptr[r], self.row_ptr[r + 1]);
                min1.iter_mut().for_each(|m| *m = BIG);
                min2.iter_mut().for_each(|m| *m = BIG);
                idx.iter_mut().for_each(|m| *m = 0);
                sgn.iter_mut().for_each(|m| *m = 0);
                for t in 0..b - a {
                    let (base, s) = (self.cols[a + t] as usize * z, self.shifts[a + t] as usize);
                    let qt = &mut q[t * z..(t + 1) * z];
                    let rt = &rmsg[(a + t) * z..(a + t + 1) * z];
                    for zz in 0..z - s {
                        qt[zz] = l[base + s + zz] - rt[zz];
                    }
                    for zz in z - s..z {
                        qt[zz] = l[base + zz + s - z] - rt[zz];
                    }
                    let tt = t as u32;
                    for zz in 0..z {
                        let bits = qt[zz].to_bits();
                        let m = f32::from_bits(bits & 0x7fff_ffff);
                        sgn[zz] ^= bits & 0x8000_0000;
                        let lt1 = m < min1[zz];
                        let lt2 = m < min2[zz];
                        min2[zz] = if lt1 {
                            min1[zz]
                        } else if lt2 {
                            m
                        } else {
                            min2[zz]
                        };
                        idx[zz] = if lt1 { tt } else { idx[zz] };
                        min1[zz] = if lt1 { m } else { min1[zz] };
                    }
                }
                for t in 0..b - a {
                    let (base, s) = (self.cols[a + t] as usize * z, self.shifts[a + t] as usize);
                    let qt = &q[t * z..(t + 1) * z];
                    let rt = &mut rmsg[(a + t) * z..(a + t + 1) * z];
                    let tt = t as u32;
                    for zz in 0..z {
                        let sign = sgn[zz] ^ (qt[zz].to_bits() & 0x8000_0000);
                        let mag = if idx[zz] == tt { min2[zz] } else { min1[zz] };
                        rt[zz] = f32::from_bits((mag * ALPHA).to_bits() | sign);
                    }
                    for zz in 0..z - s {
                        l[base + s + zz] = qt[zz] + rt[zz];
                    }
                    for zz in z - s..z {
                        l[base + zz + s - z] = qt[zz] + rt[zz];
                    }
                }
            }
            if self.syndrome_ok(&l, &mut hard) {
                finish(&l, info);
                return it;
            }
        }
        finish(&l, info);
        -1
    }
}

const NCODES: usize = BLOCK_LENGTHS.len() * RATES.len();
#[allow(clippy::declare_interior_mutable_const)]
const EMPTY: AtomicPtr<Code> = AtomicPtr::new(core::ptr::null_mut());
static CACHE: [AtomicPtr<Code>; NCODES] = [EMPTY; NCODES];

/// The code (n, rate index), built on first use and cached for the life of the program.
#[allow(unsafe_code)]
pub fn code(n: usize, rate: usize) -> Option<&'static Code> {
    let ni = BLOCK_LENGTHS.iter().position(|&x| x == n)?;
    if rate >= RATES.len() {
        return None;
    }
    let slot = &CACHE[ni * RATES.len() + rate];
    let p = slot.load(Ordering::Acquire);
    if !p.is_null() {
        // SAFETY: a non-null pointer in the cache is a leaked Box<Code> that is never freed.
        return Some(unsafe { &*p });
    }
    let fresh = Box::into_raw(Box::new(Code::build(n, rate)?));
    match slot.compare_exchange(core::ptr::null_mut(), fresh, Ordering::AcqRel, Ordering::Acquire) {
        Ok(_) => Some(unsafe { &*fresh }),
        Err(other) => {
            // another thread won: drop ours, use theirs
            drop(unsafe { Box::from_raw(fresh) });
            Some(unsafe { &*other })
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn all_codes() -> Vec<(usize, usize)> {
        let mut v = Vec::new();
        for &n in &BLOCK_LENGTHS {
            for r in 0..RATES.len() {
                if info_len(n, r).is_some() {
                    v.push((n, r));
                }
            }
        }
        v
    }

    fn rand_bits(rng: &mut XorShift64, k: usize) -> Vec<u8> {
        (0..k).map(|_| (rng.next() >> 40) as u8 & 1).collect()
    }

    #[test]
    fn dimensions() {
        assert_eq!(all_codes().len(), 20); // 2304 at 9/10 has no integer k
        assert!(info_len(2304, 6).is_none());
        assert_eq!(info_len(1920, 2), Some(960));
        assert_eq!(info_len(7680, 6), Some(6912));
        assert!(info_len(1000, 0).is_none());
        for (n, r) in all_codes() {
            let c = code(n, r).unwrap();
            assert_eq!(c.z * c.nb, n);
            assert_eq!(c.z * c.kb, c.k);
            assert!(c.mb >= 4, "n={n} r={r}");
        }
        assert_eq!(code(1920, 0).unwrap().z, 48);
        assert_eq!(code(2304, 5).unwrap().z, 48);
        assert_eq!(code(7680, 0).unwrap().z, 160);
    }

    #[test]
    fn no_four_cycles() {
        for (n, r) in all_codes() {
            let c = code(n, r).unwrap();
            let (z, nb) = (c.z as i32, c.nb);
            for r1 in 0..c.mb {
                for r2 in r1 + 1..c.mb {
                    let mut diffs: Vec<i32> = Vec::new();
                    for col in 0..nb {
                        let (a, b) = (c.base[r1 * nb + col], c.base[r2 * nb + col]);
                        if a >= 0 && b >= 0 {
                            diffs.push((a - b).rem_euclid(z));
                        }
                    }
                    let mut s = diffs.clone();
                    s.sort();
                    s.dedup();
                    assert_eq!(s.len(), diffs.len(), "4-cycle in n={n} rate={r} rows {r1},{r2}");
                }
            }
        }
    }

    #[test]
    fn dual_diagonal_parity() {
        for (n, r) in all_codes() {
            let c = code(n, r).unwrap();
            for row in 0..c.mb {
                for j in 0..c.mb {
                    let v = c.base[row * c.nb + c.kb + j];
                    if j == row || j + 1 == row {
                        assert_eq!(v, 0, "n={n} r={r}");
                    } else {
                        assert_eq!(v, -1, "n={n} r={r}");
                    }
                }
            }
            // every information column has weight >= 1, none above 7
            for col in 0..c.kb {
                let w = (0..c.mb).filter(|&row| c.base[row * c.nb + col] >= 0).count();
                assert!((1..=7).contains(&w), "n={n} r={r} col {col} weight {w}");
            }
            assert!((0..c.mb).filter(|&row| c.base[row * c.nb] >= 0).count() >= 3.min(c.mb));
        }
    }

    #[test]
    fn deterministic() {
        let a = Code::build(1920, 2).unwrap();
        let b = Code::build(1920, 2).unwrap();
        assert_eq!(a.base, b.base);
    }

    #[test]
    fn encode_is_a_codeword() {
        let mut rng = XorShift64::new(1234);
        for (n, r) in all_codes() {
            let c = code(n, r).unwrap();
            for _ in 0..2 {
                let info = rand_bits(&mut rng, c.k);
                let mut cw = vec![0u8; n];
                assert!(c.encode(&info, &mut cw));
                assert_eq!(&cw[..c.k], &info[..]);
                assert!(c.check(&cw), "n={n} r={r}");
                cw[5] ^= 1;
                assert!(!c.check(&cw));
            }
        }
    }

    #[test]
    fn decode_with_errors_every_code() {
        let mut rng = XorShift64::new(99);
        for (n, r) in all_codes() {
            let c = code(n, r).unwrap();
            let info = rand_bits(&mut rng, c.k);
            let mut cw = vec![0u8; n];
            assert!(c.encode(&info, &mut cw));
            let mut llr: Vec<f32> = cw.iter().map(|&b| if b == 0 { 3.0 } else { -3.0 }).collect();
            let flips = if r >= 5 { n / 400 } else { n / 100 };
            for i in 0..flips {
                let p = (i * 997 + 13) % n;
                llr[p] = if cw[p] == 0 { -0.5 } else { 0.5 };
            }
            let mut out = vec![0u8; c.k];
            let it = c.decode(&llr, 50, &mut out);
            assert!(it > 0, "n={n} r={r}: decoder failed ({it})");
            assert_eq!(out, info, "n={n} r={r}");
            // clean input needs no iteration
            let clean: Vec<f32> = cw.iter().map(|&b| if b == 0 { 4.0 } else { -4.0 }).collect();
            assert_eq!(c.decode(&clean, 50, &mut out), 0);
            assert_eq!(out, info);
        }
    }

    #[test]
    fn awgn_half_rate() {
        let mut rng = XorShift64::new(7);
        let c = code(1920, 2).unwrap();
        let esn0 = 1.0f32; // dB, BPSK; the limit for rate 1/2 is about -2.8 dB
        let sigma = (1.0 / (2.0 * 10f32.powf(esn0 / 10.0))).sqrt();
        let mut good = 0;
        for _ in 0..20 {
            let info = rand_bits(&mut rng, c.k);
            let mut cw = vec![0u8; c.n];
            c.encode(&info, &mut cw);
            let llr: Vec<f32> = cw
                .iter()
                .map(|&b| {
                    let u1 = ((rng.next() >> 11) as f64 + 1.0) / 9007199254740993.0;
                    let u2 = (rng.next() >> 11) as f64 / 9007199254740992.0;
                    let g = ((-2.0 * u1.ln()).sqrt() * (2.0 * core::f64::consts::PI * u2).cos()) as f32;
                    let y = if b == 0 { 1.0 } else { -1.0 } + sigma * g;
                    2.0 * y / (sigma * sigma)
                })
                .collect();
            let mut out = vec![0u8; c.k];
            if c.decode(&llr, 50, &mut out) >= 0 && out == info {
                good += 1;
            }
        }
        assert!(good >= 19, "only {good}/20 frames decoded");
    }

    #[test]
    fn hopeless_input_fails() {
        let c = code(1920, 2).unwrap();
        let mut rng = XorShift64::new(5);
        let llr: Vec<f32> = (0..c.n).map(|_| ((rng.next() >> 40) as f32 / 8388608.0 - 1.0) * 2.0).collect();
        let mut out = vec![0u8; c.k];
        assert_eq!(c.decode(&llr, 10, &mut out), -1);
        let mut small = vec![0u8; 3];
        assert_eq!(c.decode(&llr, 10, &mut small), -1);
    }
}
