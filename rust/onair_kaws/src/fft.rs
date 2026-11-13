// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Radix-2 complex FFT (f32) with a cached twiddle table; the forward transform is X[k] = sum x[n] exp(-j 2 pi k n / N).
use crate::cplx::C;
use crate::mathf;
use alloc::vec;
use alloc::vec::Vec;

pub struct Fft {
    pub n: usize,
    tw: Vec<C>,
    rev: Vec<u32>,
}
impl Fft {
    /// n must be a power of two (>= 2).
    pub fn new(n: usize) -> Fft {
        assert!(n >= 2 && n.is_power_of_two());
        let mut tw = Vec::with_capacity(n / 2);
        for k in 0..n / 2 {
            tw.push(C::polar(1.0, -2.0 * mathf::PI * k as f64 / n as f64));
        }
        let bits = n.trailing_zeros();
        let mut rev = vec![0u32; n];
        for i in 0..n {
            rev[i] = ((i as u32).reverse_bits()) >> (32 - bits);
        }
        Fft { n, tw, rev }
    }
    fn run(&self, a: &mut [C], inverse: bool) {
        let n = self.n;
        for i in 0..n {
            let j = self.rev[i] as usize;
            if j > i { a.swap(i, j); }
        }
        let mut len = 2;
        while len <= n {
            let step = n / len;
            let half = len / 2;
            let mut i = 0;
            while i < n {
                for k in 0..half {
                    let mut w = self.tw[k * step];
                    if inverse { w = w.conj(); }
                    let u = a[i + k];
                    let v = a[i + k + half] * w;
                    a[i + k] = u + v;
                    a[i + k + half] = u - v;
                }
                i += len;
            }
            len <<= 1;
        }
    }
    pub fn forward(&self, a: &mut [C]) { self.run(a, false); }
    /// Unnormalised inverse (sum with exp(+j...)).
    pub fn inverse(&self, a: &mut [C]) { self.run(a, true); }
}

/// Smallest power of two >= n.
pub fn next_pow2(n: usize) -> usize { n.next_power_of_two().max(2) }

/// Cross-correlation of `x` with the reference `r`: out[t] = sum_n x[t + n] conj(r[n]) for t = 0..=len(x)-len(r) (FFT based).
pub fn correlate(x: &[C], r: &[C]) -> Vec<C> {
    if x.len() < r.len() || r.is_empty() { return Vec::new(); }
    let n = next_pow2(x.len() + r.len());
    let f = Fft::new(n);
    let mut a = vec![C::ZERO; n];
    a[..x.len()].copy_from_slice(x);
    let mut b = vec![C::ZERO; n];
    b[..r.len()].copy_from_slice(r);
    f.forward(&mut a);
    f.forward(&mut b);
    for i in 0..n { a[i] = a[i] * b[i].conj(); }
    f.inverse(&mut a);
    let inv = 1.0 / n as f32;
    let m = x.len() - r.len() + 1;
    (0..m).map(|t| a[t].scale(inv)).collect()
}
