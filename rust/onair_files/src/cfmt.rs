// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C `printf` number formats: `%.<P>g` and `%.<N>f` of a finite double through the C library (`ffi::cfn`, so the text equals what the C++ `snprintf` gave), and a byte
//! sink that counts what did not fit.
use crate::ffi::cfn;
use core::fmt::Write;

/// Collects bytes into a caller buffer; `len` counts every byte offered, so the caller learns the size needed even when the buffer was too small.
pub struct Sink<'a> {
    /// Destination (may be empty to only measure).
    pub buf: &'a mut [u8],
    /// Bytes offered so far.
    pub len: usize,
}

impl<'a> Sink<'a> {
    /// A sink over `buf`.
    pub fn new(buf: &'a mut [u8]) -> Sink<'a> {
        Sink { buf, len: 0 }
    }

    /// Appends bytes.
    pub fn put(&mut self, b: &[u8]) {
        for &c in b {
            if self.len < self.buf.len() {
                self.buf[self.len] = c;
            }
            self.len += 1;
        }
    }
}

impl Write for Sink<'_> {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        self.put(s.as_bytes());
        Ok(())
    }
}

/// Appends `x` as `%.<prec>g` (9 or 10). Non-finite values print as 0 (callers never pass them: the JSON would be invalid).
pub fn put_g(out: &mut Sink, x: f64, prec: u8) {
    let x = if x.is_finite() { x } else { 0.0 };
    let mut b = [0u8; 64];
    let n = cfn::format_g(prec, x, &mut b);
    out.put(&b[..n]);
}

/// Appends `x` as `%.1f` (non-finite prints as 0).
pub fn put_f1(out: &mut Sink, x: f64) {
    let x = if x.is_finite() { x } else { 0.0 };
    let mut b = [0u8; 64];
    let n = cfn::format_f(1, x, &mut b);
    out.put(&b[..n]);
}
