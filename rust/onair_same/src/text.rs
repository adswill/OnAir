// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! A tiny bounded text writer: the protocol crates do not allocate, so text is built in a caller buffer.

/// Appends to a byte buffer; once something does not fit, `overflow` is set and nothing more is written.
pub struct Buf<'a> {
    out: &'a mut [u8],
    len: usize,
    /// True when the buffer was too small for everything pushed so far.
    pub overflow: bool,
}

impl<'a> Buf<'a> {
    /// A writer over `out`, empty.
    pub fn new(out: &'a mut [u8]) -> Self {
        Buf { out, len: 0, overflow: false }
    }
    /// Bytes written so far.
    pub fn len(&self) -> usize {
        self.len
    }
    /// True when nothing was written.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
    /// Appends bytes.
    pub fn bytes(&mut self, s: &[u8]) {
        if self.overflow || self.len + s.len() > self.out.len() {
            self.overflow = true;
            return;
        }
        self.out[self.len..self.len + s.len()].copy_from_slice(s);
        self.len += s.len();
    }
    /// Appends a string.
    pub fn str(&mut self, s: &str) {
        self.bytes(s.as_bytes());
    }
    /// Appends a string without its trailing NUL (the tables keep names NUL-terminated for the C ABI).
    pub fn cstr(&mut self, s: &str) {
        self.bytes(strip_nul(s).as_bytes());
    }
    /// Appends one byte.
    pub fn byte(&mut self, b: u8) {
        self.bytes(&[b]);
    }
    /// Appends a decimal number, at least `width` digits (zero padded), with a leading minus when negative (like `%0*d`).
    pub fn dec(&mut self, v: i64, width: usize) {
        let mut tmp = [0u8; 24];
        let mut i = tmp.len();
        let mut u = v.unsigned_abs();
        loop {
            i -= 1;
            tmp[i] = b'0' + (u % 10) as u8;
            u /= 10;
            if u == 0 {
                break;
            }
        }
        let digits = tmp.len() - i;
        if v < 0 {
            self.byte(b'-');
        }
        for _ in digits..width {
            self.byte(b'0');
        }
        self.bytes(&tmp[i..]);
    }
    /// The first byte (used to capitalise a finished text).
    pub fn first_mut(&mut self) -> Option<&mut u8> {
        if self.len > 0 && !self.overflow {
            Some(&mut self.out[0])
        } else {
            None
        }
    }
}

/// A table string without its terminating NUL.
pub fn strip_nul(s: &str) -> &str {
    s.strip_suffix('\0').unwrap_or(s)
}
