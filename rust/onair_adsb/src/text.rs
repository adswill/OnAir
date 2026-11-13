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
    /// Appends one byte.
    pub fn byte(&mut self, b: u8) {
        self.bytes(&[b]);
    }
}
