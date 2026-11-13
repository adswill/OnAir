// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! xorshift64 (shifts 13, 7, 17): the fixed generator behind the deterministic code tables.

#[derive(Clone, Copy, Debug)]
pub struct XorShift64 {
    pub s: u64,
}

impl XorShift64 {
    pub fn new(seed: u64) -> Self {
        XorShift64 { s: if seed == 0 { 0x9E37_79B9_7F4A_7C15 } else { seed } }
    }
    #[allow(clippy::should_implement_trait)]
    pub fn next(&mut self) -> u64 {
        let mut x = self.s;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.s = x;
        x
    }
    /// Uniform-enough value in 0..n (n > 0).
    pub fn below(&mut self, n: u32) -> u32 {
        ((self.next() >> 11) % n as u64) as u32
    }
}
