// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! xorshift64 generator with uniform and Gaussian draws (deterministic simulations).
use crate::mathf;

#[derive(Clone, Copy, Debug)]
pub struct Rng {
    s: u64,
}
impl Rng {
    pub fn new(seed: u64) -> Rng {
        let mut r = Rng { s: if seed == 0 { 0x9E37_79B9_7F4A_7C15 } else { seed } };
        for _ in 0..8 { r.next_u64(); }
        r
    }
    pub fn next_u64(&mut self) -> u64 {
        let mut x = self.s;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.s = x;
        x
    }
    /// Uniform in [0, 1).
    pub fn uniform(&mut self) -> f64 { (self.next_u64() >> 11) as f64 / (1u64 << 53) as f64 }
    /// Uniform in 0..n.
    pub fn below(&mut self, n: u32) -> u32 { ((self.next_u64() >> 11) % n as u64) as u32 }
    /// Standard normal (Box-Muller).
    pub fn gauss(&mut self) -> f64 {
        let u1 = self.uniform().max(1e-300);
        let u2 = self.uniform();
        mathf::sqrt(-2.0 * mathf::ln(u1)) * mathf::cos(2.0 * mathf::PI * u2)
    }
    /// Uniform in [a, b).
    pub fn range(&mut self, a: f64, b: f64) -> f64 { a + (b - a) * self.uniform() }
}
