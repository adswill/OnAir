// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Bit reader over one bit per byte, as the C++ `R` helper of tetra_pdu.cpp: reads past the end give 0 and clear `ok`.
//! A byte is OR-ed in as it is (not masked to one bit), exactly like the C++ reader, so odd input behaves identically.

/// MSB first reader over `b[p..end]`.
pub struct Reader<'a> {
    pub b: &'a [u8],
    pub p: usize,
    pub end: usize,
    pub ok: bool,
}

impl<'a> Reader<'a> {
    /// Reader over `b[from..min(to, b.len())]`.
    pub fn new(b: &'a [u8], from: usize, to: usize) -> Self {
        Reader { b, p: from, end: to.min(b.len()), ok: true }
    }
    /// Read `n` (at most 64) bits; past the end: 0, `ok` cleared and the position moved to the end.
    pub fn get(&mut self, n: usize) -> u64 {
        if self.p.saturating_add(n) > self.end {
            self.ok = false;
            self.p = self.end;
            return 0;
        }
        let mut v = 0u64;
        for _ in 0..n {
            v = (v << 1) | self.b[self.p] as u64;
            self.p += 1;
        }
        v
    }
    /// One bit as a flag.
    pub fn bit(&mut self) -> bool {
        self.get(1) != 0
    }
    /// Bits left.
    pub fn left(&self) -> usize {
        self.end.saturating_sub(self.p)
    }
}
