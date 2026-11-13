// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Bit helpers of the DMR layer: bits are one per byte (0 or 1) in on-air order; integers hold their first bit in the top position.

/// Appends bits to a fixed buffer; writes past the end are dropped (callers size their buffers exactly).
pub struct Out<'a> {
    b: &'a mut [u8],
    /// Number of bits appended (also those that did not fit).
    pub n: usize,
}

impl<'a> Out<'a> {
    /// A writer over `b`, starting empty.
    pub fn new(b: &'a mut [u8]) -> Self {
        Out { b, n: 0 }
    }
    /// Appends the `n` low bits of `v`, most significant first.
    pub fn put(&mut self, v: u64, n: u32) {
        for i in (0..n).rev() {
            if self.n < self.b.len() {
                self.b[self.n] = ((v >> i) & 1) as u8;
            }
            self.n += 1;
        }
    }
    /// Appends the bits of each byte of `d`, most significant first.
    pub fn put_bytes(&mut self, d: &[u8]) {
        for &x in d {
            self.put(u64::from(x), 8);
        }
    }
    /// Appends `n` copies of `v` (0 or 1).
    pub fn fill(&mut self, v: u8, n: usize) {
        for _ in 0..n {
            if self.n < self.b.len() {
                self.b[self.n] = v;
            }
            self.n += 1;
        }
    }
    /// Appends the bits of `s`.
    pub fn put_slice(&mut self, s: &[u8]) {
        for &x in s {
            if self.n < self.b.len() {
                self.b[self.n] = x;
            }
            self.n += 1;
        }
    }
}

/// `n` bits (at most 64) of `b` from bit `pos`, first bit on top; bits past the end read as 0.
pub fn get_bits(b: &[u8], pos: usize, n: usize) -> u64 {
    let mut v = 0u64;
    for i in 0..n {
        v = (v << 1) | if pos + i < b.len() { u64::from(b[pos + i] & 1) } else { 0 };
    }
    v
}

/// Packs `nbits / 8` bytes out of `b` starting at bit `pos` (`out` receives them; bits past the end read as 0).
pub fn bits_to_bytes(b: &[u8], pos: usize, nbits: usize, out: &mut [u8]) {
    for i in 0..nbits / 8 {
        if i < out.len() {
            out[i] = get_bits(b, pos + i * 8, 8) as u8;
        }
    }
}

/// Population count of a 32-bit word.
pub fn popcount32(v: u32) -> i32 {
    v.count_ones() as i32
}
