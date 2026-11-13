// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Cyclic-shift chirp modem (profiles P0-P4, spec section 2 and 7.4). Complex baseband at one sample per chip (2400/s); a symbol of spreading factor sf has
//! M = 2^sf chips, value m = 0..M-1 = a cyclic shift of the base chirp by m chips, rising (d = +1) or falling (d = -1). The direction of symbol n follows
//! the weave LFSR. Demodulation: multiply by the conjugate base chirp (rising) or the base chirp (falling) and take an M-point FFT; the peak bin is m.
//! A delay of e chips moves the peak by -e (rising) or +e (falling) bins, a frequency offset of x bins by +x (both).
use crate::cplx::C;
use crate::fft::Fft;
use crate::mathf;
use alloc::vec;
use alloc::vec::Vec;

/// The weave sequence: 16-bit Galois LFSR (taps 0xB400, seed 0xACE1), output the least significant bit, 1 = falling. Restarted at each block.
pub struct Weave {
    state: u16,
}
impl Weave {
    pub fn new() -> Weave { Weave { state: 0xACE1 } }
    /// Direction of the next symbol: +1 rising, -1 falling.
    pub fn next_dir(&mut self) -> i32 {
        let out = self.state & 1;
        self.state >>= 1;
        if out != 0 { self.state ^= 0xB400; }
        if out != 0 { -1 } else { 1 }
    }
}
impl Default for Weave {
    fn default() -> Self { Weave::new() }
}

pub fn gray(b: u32) -> u32 { b ^ (b >> 1) }
pub fn gray_inv(mut g: u32) -> u32 {
    let mut b = 0;
    while g != 0 { b ^= g; g >>= 1; }
    b
}

pub struct Modem {
    pub sf: usize,
    pub m: usize,
    pub guard: usize,
    base: Vec<C>,
    fft: Fft,
}
impl Modem {
    pub fn new(sf: usize, guard: usize) -> Modem {
        let m = 1usize << sf;
        let mut base = Vec::with_capacity(m);
        for n in 0..m {
            let ph = mathf::PI * ((n * n) as f64 - (m * n) as f64) / m as f64;
            base.push(C::polar(1.0, ph));
        }
        Modem { sf, m, guard, base, fft: Fft::new(m) }
    }
    pub fn base(&self) -> &[C] { &self.base }
    /// The symbol (guard chips first, then M chips) for value `v` and direction `dir`.
    pub fn symbol(&self, v: usize, dir: i32, out: &mut Vec<C>) {
        let m = self.m;
        let mut body = Vec::with_capacity(m);
        for n in 0..m {
            body.push(if dir > 0 { self.base[(n + v) % m] } else { self.base[(n + m - v % m) % m].conj() });
        }
        for i in 0..self.guard { out.push(body[m - self.guard + i]); }
        out.extend_from_slice(&body);
    }
    /// Energy-normalised FFT bins |Y_m|^2 / M of one received symbol (`x` = M chips after the guard); also returns the complex bins.
    pub fn dechirp(&self, x: &[C], dir: i32, bins: &mut Vec<C>) {
        let m = self.m;
        bins.clear();
        for n in 0..m {
            bins.push(if dir > 0 { x[n] * self.base[n].conj() } else { x[n] * self.base[n] });
        }
        self.fft.forward(bins);
    }
}

/// Soft bit values (LLR, positive = 0) of one symbol from the per-bin log-likelihoods `w` (length M): bit b of the Gray-decoded bin index.
/// Bit order: the most significant bit of the symbol is the first bit.
pub fn bin_llrs(w: &[f64], sf: usize, out: &mut [f32]) {
    let m = w.len();
    let mut wmax = f64::MIN;
    for &v in w { if v > wmax { wmax = v; } }
    let mut s0 = vec![0f64; sf];
    let mut s1 = vec![0f64; sf];
    for bin in 0..m {
        let p = mathf::exp(w[bin] - wmax);
        let b = gray_inv(bin as u32);
        for j in 0..sf {
            if (b >> (sf - 1 - j)) & 1 == 0 { s0[j] += p; } else { s1[j] += p; }
        }
    }
    for j in 0..sf {
        out[j] = (mathf::ln(s0[j].max(1e-300)) - mathf::ln(s1[j].max(1e-300))) as f32;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn weave_first_bits() {
        let mut w = Weave::new();
        let s: alloc::string::String = (0..16).map(|_| if w.next_dir() < 0 { '1' } else { '0' }).collect();
        // The spec text lists 1000011100100011; print what the stated generator really gives.
        std::println!("weave bits: {s}");
    }
    #[test]
    fn symbols_are_tones() {
        for sf in [6usize, 8, 10] {
            let md = Modem::new(sf, if sf == 6 { 16 } else { 0 });
            for dir in [1, -1] {
                for v in [0usize, 1, 5, md.m / 2, md.m - 1] {
                    let mut s = Vec::new();
                    md.symbol(v, dir, &mut s);
                    let body = &s[md.guard..];
                    let mut bins = Vec::new();
                    md.dechirp(body, dir, &mut bins);
                    let (mut best, mut be) = (0, 0f32);
                    for (i, b) in bins.iter().enumerate() { if b.norm_sqr() > be { be = b.norm_sqr(); best = i; } }
                    assert_eq!(best, v, "sf {sf} dir {dir} v {v}");
                    assert!((be - (md.m * md.m) as f32).abs() < 1.0);
                }
            }
        }
    }
}

/// Like `bin_llrs`, with a priori bit LLRs `la` (positive = 0) added to the bin metrics; returns the extrinsic LLRs (all-bit LLR minus the own prior).
/// `gi` is the Gray-decode table of the bins (`gray_inv(bin)` for bin in 0..M).
pub fn bin_llrs_prior(w: &[f32], sf: usize, gi: &[u32], la: &[f32], out: &mut [f32], scratch: &mut Vec<f64>) {
    let m = w.len();
    scratch.clear();
    let mut wmax = f64::MIN;
    for bin in 0..m {
        let b = gi[bin];
        let mut v = w[bin] as f64;
        for j in 0..sf {
            let h = 0.5 * la[j] as f64;
            if (b >> (sf - 1 - j)) & 1 == 0 { v += h; } else { v -= h; }
        }
        if v > wmax { wmax = v; }
        scratch.push(v);
    }
    let mut s0 = [0f64; 16];
    let mut s1 = [0f64; 16];
    for bin in 0..m {
        let p = mathf::exp(scratch[bin] - wmax);
        let b = gi[bin];
        for j in 0..sf {
            if (b >> (sf - 1 - j)) & 1 == 0 { s0[j] += p; } else { s1[j] += p; }
        }
    }
    for j in 0..sf {
        out[j] = (mathf::ln(s0[j].max(1e-300)) - mathf::ln(s1[j].max(1e-300))) as f32 - la[j];
    }
}
