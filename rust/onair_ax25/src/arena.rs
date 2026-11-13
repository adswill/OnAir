// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! A bounded byte arena for the text results of the APRS parser: strings are appended once and handed out as (offset, length) spans, so the crate
//! needs no allocation. Also `C`-style number formatting (`%.Nf`) through the C library, so the digits match the C++ reference exactly.

/// A string inside the arena (byte offset and length).
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct Span {
    /// Byte offset from the start of the arena.
    pub off: u32,
    /// Length in bytes.
    pub len: u32,
}

/// Append-only byte arena over a caller buffer; `overflow` is set when something did not fit (nothing more is written then).
pub struct Arena<'a> {
    buf: &'a mut [u8],
    len: usize,
    /// True when the buffer was too small for everything pushed so far.
    pub overflow: bool,
}

impl<'a> Arena<'a> {
    /// An empty arena over `buf`.
    pub fn new(buf: &'a mut [u8]) -> Self {
        Arena { buf, len: 0, overflow: false }
    }
    /// Current length (the offset of the next string).
    pub fn mark(&self) -> usize {
        self.len
    }
    /// Drops everything after `len` bytes (only when nothing overflowed).
    pub fn truncate(&mut self, len: usize) {
        if len <= self.len {
            self.len = len;
        }
    }
    /// Appends bytes without making a span.
    pub fn put(&mut self, s: &[u8]) {
        if self.overflow || self.len + s.len() > self.buf.len() {
            self.overflow = true;
            return;
        }
        self.buf[self.len..self.len + s.len()].copy_from_slice(s);
        self.len += s.len();
    }
    /// Appends one byte.
    pub fn byte(&mut self, b: u8) {
        self.put(&[b]);
    }
    /// Appends a copy of an earlier string.
    pub fn put_span(&mut self, s: Span) {
        let (o, n) = (s.off as usize, s.len as usize);
        if self.overflow || o + n > self.len || self.len + n > self.buf.len() {
            self.overflow = true;
            return;
        }
        self.buf.copy_within(o..o + n, self.len);
        self.len += n;
    }
    /// The span from `start` to the current length.
    pub fn since(&self, start: usize) -> Span {
        Span { off: start as u32, len: (self.len - start) as u32 }
    }
    /// Appends bytes as a string of its own.
    pub fn push(&mut self, s: &[u8]) -> Span {
        let m = self.mark();
        self.put(s);
        self.since(m)
    }
    /// Appends a decimal with `prec` digits after the point (C `%.{prec}f`, prec 0 to 4), as C's snprintf with a 64 byte buffer would write it.
    pub fn fixed(&mut self, prec: u8, v: f64) {
        let mut tmp = [0u8; 64];
        let n = crate::ffi::cfn::format_f(prec, v, &mut tmp);
        self.put(&tmp[..n]);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn spans_and_numbers() {
        let mut b = [0u8; 64];
        let mut a = Arena::new(&mut b);
        let s = a.push(b"abc");
        a.put_span(s);
        a.fixed(4, 25.2);
        a.fixed(0, 2.5);
        let all = a.since(0);
        assert_eq!(all.len as usize, "abcabc25.20002".len());
        assert!(!a.overflow);
        let mut tiny = [0u8; 2];
        let mut t = Arena::new(&mut tiny);
        t.put(b"xyz");
        assert!(t.overflow);
    }
}
