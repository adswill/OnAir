// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! KAWS LDPC codes: systematic quasi-cyclic IRA codes with a dual-diagonal parity part (the construction of the Kris family, rust/onair_kris), built for the
//! short block lengths of KAWS (n = 512 .. 1728) from a fixed-seed generator, so every build produces the same matrix. Decoder: layered normalised min-sum
//! (alpha 0.8), f32 LLRs, positive = bit 0 more likely. Matrices are a v0.1 design (spec open item 1): the FER curves are measured, not designed.
use crate::rng::Rng;
use alloc::boxed::Box;
use alloc::vec;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicPtr, Ordering};

pub const ALPHA: f32 = 0.8;
/// Degree profile of the information columns (percent of heavy columns, heavy weight, light weight); tuning knobs of the code construction.
pub static TUNE_HEAVY_PCT: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(100);
pub static TUNE_W_HEAVY: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(4);
pub static TUNE_W_LIGHT: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(4);
const BIG: f32 = 1.0e4;

fn gcd(a: usize, b: usize) -> usize { if b == 0 { a } else { gcd(b, a % b) } }

/// Lifting size: the largest divisor of gcd(n, k) that is at most 24.
pub fn lifting(n: usize, k: usize) -> usize {
    let g = gcd(n, k);
    let mut d = 24;
    while d > 1 {
        if g % d == 0 { return d; }
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
    pub base: Vec<i32>,
    row_ptr: Vec<usize>,
    info_end: Vec<usize>,
    cols: Vec<u32>,
    shifts: Vec<u32>,
    max_deg: usize,
}

fn conflict(used: &[bool], mb: usize, z: usize, r1: usize, s1: usize, r2: usize, s2: usize) -> bool {
    if r1 < r2 { used[(r1 * mb + r2) * z + (s1 + z - s2) % z] } else { used[(r2 * mb + r1) * z + (s2 + z - s1) % z] }
}
fn mark(used: &mut [bool], mb: usize, z: usize, r1: usize, s1: usize, r2: usize, s2: usize) {
    if r1 < r2 { used[(r1 * mb + r2) * z + (s1 + z - s2) % z] = true; } else { used[(r2 * mb + r1) * z + (s2 + z - s1) % z] = true; }
}

impl Code {
    pub fn build(n: usize, k: usize) -> Option<Code> {
        let z = lifting(n, k);
        if n % z != 0 || k % z != 0 || k >= n { return None; }
        let (nb, kb) = (n / z, k / z);
        let mb = nb - kb;
        if mb < 2 { return None; }
        let mut rng = Rng::new(0x9E37_79B9_7F4A_7C15 ^ ((n as u64) << 20) ^ ((k as u64) << 4) ^ 0x4B41_5753);
        let mut used = vec![false; mb * mb * z];
        for j in 0..mb - 1 {
            used[(j * mb + j + 1) * z] = true;
        }
        let mut base = vec![-1i32; mb * nb];
        let mut deg = vec![0u32; mb];
        let heavy = core::cmp::max(1, kb * TUNE_HEAVY_PCT.load(Ordering::Relaxed) as usize / 100);
        let (w_heavy, w_light) = (TUNE_W_HEAVY.load(Ordering::Relaxed) as usize, TUNE_W_LIGHT.load(Ordering::Relaxed) as usize);
        for c in 0..kb {
            let mut w = core::cmp::min(if c < heavy { w_heavy } else { w_light }, mb);
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
                        let allowed: Vec<usize> = (0..z).filter(|&s| (0..i).all(|j| !conflict(&used, mb, z, rows[j], sh[j], r, s))).collect();
                        if allowed.is_empty() { ok = false; break; }
                        sh.push(allowed[rng.below(allowed.len() as u32) as usize]);
                    }
                    if ok { placed = Some((rows, sh)); break; }
                }
                if let Some((rows, sh)) = placed {
                    for i in 0..rows.len() {
                        for j in 0..i { mark(&mut used, mb, z, rows[j], sh[j], rows[i], sh[i]); }
                        base[rows[i] * nb + c] = sh[i] as i32;
                        deg[rows[i]] += 1;
                    }
                    break;
                }
                w -= 1;
            }
        }
        for r in 0..mb {
            base[r * nb + kb + r] = 0;
            if r + 1 < mb { base[(r + 1) * nb + kb + r] = 0; }
        }
        let (mut row_ptr, mut info_end, mut cols, mut shifts) = (vec![0usize], Vec::new(), Vec::new(), Vec::new());
        let mut max_deg = 0;
        for r in 0..mb {
            for c in 0..kb {
                let s = base[r * nb + c];
                if s >= 0 { cols.push(c as u32); shifts.push(s as u32); }
            }
            info_end.push(cols.len());
            for c in kb..nb {
                let s = base[r * nb + c];
                if s >= 0 { cols.push(c as u32); shifts.push(s as u32); }
            }
            max_deg = core::cmp::max(max_deg, cols.len() - row_ptr[r]);
            row_ptr.push(cols.len());
        }
        Some(Code { n, k, z, nb, kb, mb, base, row_ptr, info_end, cols, shifts, max_deg })
    }

    /// Systematic encode: `info` k bits (one per byte), `cw` n bits (information first, then parity).
    pub fn encode(&self, info: &[u8], cw: &mut [u8]) -> bool {
        if info.len() != self.k || cw.len() != self.n { return false; }
        let (z, k) = (self.z, self.k);
        for i in 0..k { cw[i] = info[i] & 1; }
        let mut acc = vec![0u8; z];
        for r in 0..self.mb {
            acc.iter_mut().for_each(|a| *a = 0);
            for e in self.row_ptr[r]..self.info_end[r] {
                let (b, s) = (self.cols[e] as usize * z, self.shifts[e] as usize);
                for zz in 0..z - s { acc[zz] ^= cw[b + s + zz]; }
                for zz in z - s..z { acc[zz] ^= cw[b + zz + s - z]; }
            }
            if r > 0 {
                let prev = k + (r - 1) * z;
                for zz in 0..z { acc[zz] ^= cw[prev + zz]; }
            }
            cw[k + r * z..k + (r + 1) * z].copy_from_slice(&acc);
        }
        true
    }

    pub fn check(&self, bits: &[u8]) -> bool {
        if bits.len() != self.n { return false; }
        let z = self.z;
        let mut acc = vec![0u8; z];
        for r in 0..self.mb {
            acc.iter_mut().for_each(|a| *a = 0);
            for e in self.row_ptr[r]..self.row_ptr[r + 1] {
                let (b, s) = (self.cols[e] as usize * z, self.shifts[e] as usize);
                for zz in 0..z - s { acc[zz] ^= bits[b + s + zz] & 1; }
                for zz in z - s..z { acc[zz] ^= bits[b + zz + s - z] & 1; }
            }
            if acc.iter().any(|&a| a != 0) { return false; }
        }
        true
    }

    fn syndrome_ok(&self, l: &[f32], hard: &mut [u8]) -> bool {
        for (h, v) in hard.iter_mut().zip(l) { *h = (v.to_bits() >> 31) as u8; }
        self.check(hard)
    }

    /// Layered normalised min-sum. Returns the iterations used (0 = the input already satisfied every check) or -1 when the checks never passed.
    /// `info` receives the k hard decisions (also on failure).
    pub fn decode(&self, llr: &[f32], max_iter: i32, info: &mut [u8]) -> i32 {
        self.decode_post(llr, max_iter, info, None)
    }

    /// As `decode`, optionally writing the n posterior LLRs (channel + decoder) to `post`.
    pub fn decode_post(&self, llr: &[f32], max_iter: i32, info: &mut [u8], mut post: Option<&mut [f32]>) -> i32 {
        if llr.len() != self.n || info.len() != self.k { return -1; }
        let z = self.z;
        let mut l: Vec<f32> = llr.iter().map(|&v| if v != v { 0.0 } else if v > 30.0 { 30.0 } else if v < -30.0 { -30.0 } else { v }).collect();
        let mut hard = vec![0u8; self.n];
        let mut finish = |l: &[f32], info: &mut [u8]| {
            for (i, o) in info.iter_mut().enumerate() { *o = (l[i].to_bits() >> 31) as u8; }
            if let Some(pp) = post.as_deref_mut() { pp.copy_from_slice(l); }
        };
        if self.syndrome_ok(&l, &mut hard) { finish(&l, info); return 0; }
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
                    for zz in 0..z {
                        let src = base + if zz + s < z { zz + s } else { zz + s - z };
                        let v = l[src] - rmsg[(a + t) * z + zz];
                        q[t * z + zz] = v;
                        let bits = v.to_bits();
                        let m = f32::from_bits(bits & 0x7fff_ffff);
                        sgn[zz] ^= bits & 0x8000_0000;
                        if m < min1[zz] { min2[zz] = min1[zz]; min1[zz] = m; idx[zz] = t as u32; } else if m < min2[zz] { min2[zz] = m; }
                    }
                }
                for t in 0..b - a {
                    let (base, s) = (self.cols[a + t] as usize * z, self.shifts[a + t] as usize);
                    for zz in 0..z {
                        let qv = q[t * z + zz];
                        let sign = sgn[zz] ^ (qv.to_bits() & 0x8000_0000);
                        let mag = if idx[zz] == t as u32 { min2[zz] } else { min1[zz] };
                        let rv = f32::from_bits((mag * ALPHA).to_bits() | sign);
                        rmsg[(a + t) * z + zz] = rv;
                        let dst = base + if zz + s < z { zz + s } else { zz + s - z };
                        l[dst] = qv + rv;
                    }
                }
            }
            if self.syndrome_ok(&l, &mut hard) { finish(&l, info); return it; }
        }
        finish(&l, info);
        -1
    }
}

const NSLOT: usize = 8;
#[allow(clippy::declare_interior_mutable_const)]
const EMPTY: AtomicPtr<Code> = AtomicPtr::new(core::ptr::null_mut());
static CACHE: [AtomicPtr<Code>; NSLOT] = [EMPTY; NSLOT];

/// The code of profile `id` (0..7), built on first use and cached for the life of the program.
#[allow(unsafe_code)]
pub fn profile_code(id: usize) -> Option<&'static Code> {
    let p = crate::profile::get(id)?;
    let slot = &CACHE[id];
    let cur = slot.load(Ordering::Acquire);
    if !cur.is_null() {
        // SAFETY: a non-null pointer in the cache is a leaked Box<Code> that is never freed.
        return Some(unsafe { &*cur });
    }
    let fresh = Box::into_raw(Box::new(Code::build(p.n, p.k)?));
    match slot.compare_exchange(core::ptr::null_mut(), fresh, Ordering::AcqRel, Ordering::Acquire) {
        // SAFETY: both pointers come from leaked boxes; the loser is dropped before use.
        Ok(_) => Some(unsafe { &*fresh }),
        Err(other) => {
            drop(unsafe { Box::from_raw(fresh) });
            Some(unsafe { &*other })
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn all_profiles_encode_and_check() {
        for id in 0..8 {
            let c = profile_code(id).unwrap();
            let mut r = Rng::new(7 + id as u64);
            let info: Vec<u8> = (0..c.k).map(|_| (r.next_u64() & 1) as u8).collect();
            let mut cw = vec![0u8; c.n];
            assert!(c.encode(&info, &mut cw));
            assert!(c.check(&cw), "profile {id}");
            let llr: Vec<f32> = cw.iter().map(|&b| if b == 0 { 4.0 } else { -4.0 }).collect();
            let mut out = vec![0u8; c.k];
            assert_eq!(c.decode(&llr, 20, &mut out), 0);
            assert_eq!(out, info);
        }
    }
}
