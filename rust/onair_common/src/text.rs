// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! A bounded text writer for the protocol crates (they do not allocate): decimal and hex numbers, bytes and strings into a caller buffer, with the
//! padding rules of C `printf` (`%u`, `%d`, `%02x`, `%08X`, `%016llX`). Floating point goes through a function the crate supplies (its C library `snprintf`),
//! so digits are identical to the C++ reference.

/// Formats `v` as C `%.{prec}f` into the 64 byte buffer and returns the length written (what a C `snprintf` with that buffer would give).
pub type FormatF = fn(prec: u8, v: f64, out: &mut [u8; 64]) -> usize;

/// Appends to a byte buffer; once something does not fit, `overflow` is set, the text is cut at the buffer end and nothing more is written.
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
    /// A writer over `out` that continues after the first `len` bytes (already written).
    pub fn resume(out: &'a mut [u8], len: usize) -> Self {
        let len = len.min(out.len());
        Buf { out, len, overflow: false }
    }
    /// Bytes written so far.
    pub fn len(&self) -> usize {
        self.len
    }
    /// True when nothing was written.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
    /// The bytes written so far.
    pub fn as_bytes(&self) -> &[u8] {
        &self.out[..self.len]
    }
    /// Appends bytes (as many as fit; the rest sets `overflow`).
    pub fn bytes(&mut self, s: &[u8]) {
        let room = self.out.len() - self.len;
        let n = s.len().min(room);
        self.out[self.len..self.len + n].copy_from_slice(&s[..n]);
        self.len += n;
        if n < s.len() {
            self.overflow = true;
        }
    }
    /// Appends a string.
    pub fn str(&mut self, s: &str) {
        self.bytes(s.as_bytes());
    }
    /// Appends one byte.
    pub fn byte(&mut self, b: u8) {
        self.bytes(&[b]);
    }
    /// Appends an unsigned decimal (`%u`, `%zu`, `%llu`).
    pub fn udec(&mut self, mut v: u64) {
        let mut t = [0u8; 20];
        let mut i = t.len();
        loop {
            i -= 1;
            t[i] = b'0' + (v % 10) as u8;
            v /= 10;
            if v == 0 {
                break;
            }
        }
        self.bytes(&t[i..]);
    }
    /// Appends a signed decimal (`%d`).
    pub fn idec(&mut self, v: i64) {
        if v < 0 {
            self.byte(b'-');
        }
        self.udec(v.unsigned_abs());
    }
    /// Appends hex digits, zero padded to `width` (`%02x`, `%08X`, `%016llX`).
    pub fn hex(&mut self, v: u64, width: usize, upper: bool) {
        let digits: &[u8; 16] = if upper { b"0123456789ABCDEF" } else { b"0123456789abcdef" };
        let mut t = [0u8; 16];
        let mut n = 0;
        let mut x = v;
        loop {
            t[n] = digits[(x & 15) as usize];
            n += 1;
            x >>= 4;
            if x == 0 {
                break;
            }
        }
        for _ in n..width.min(16) {
            self.byte(b'0');
        }
        for i in (0..n).rev() {
            self.byte(t[i]);
        }
    }
    /// Appends `v` as C `%.{prec}f` with the crate's formatter.
    pub fn fixed(&mut self, f: FormatF, prec: u8, v: f64) {
        let mut tmp = [0u8; 64];
        let n = f(prec, v, &mut tmp);
        self.bytes(&tmp[..n]);
    }
}

/// A byte string of at most `N` bytes.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Str<const N: usize> {
    /// Length used.
    pub len: u16,
    /// The bytes (not NUL terminated).
    pub b: [u8; N],
}

impl<const N: usize> Default for Str<N> {
    fn default() -> Self {
        Str { len: 0, b: [0; N] }
    }
}

impl<const N: usize> Str<N> {
    /// The bytes.
    pub fn as_bytes(&self) -> &[u8] {
        &self.b[..self.len as usize]
    }
    /// Replaces the text (cut at `N` bytes).
    pub fn set(&mut self, s: &[u8]) {
        let n = s.len().min(N);
        self.b[..n].copy_from_slice(&s[..n]);
        self.len = n as u16;
    }
    /// Builds the text with a writer.
    pub fn write(&mut self, f: impl FnOnce(&mut Buf)) {
        let mut w = Buf::new(&mut self.b);
        f(&mut w);
        self.len = w.len() as u16;
    }
    /// Appends text built by a writer that sees the existing text (so it can tell whether to add a separator).
    pub fn append(&mut self, f: impl FnOnce(&mut Buf)) {
        let mut w = Buf::resume(&mut self.b, self.len as usize);
        f(&mut w);
        self.len = w.len() as u16;
    }
    /// Empties the text.
    pub fn clear(&mut self) {
        self.len = 0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn numbers() {
        let mut b = [0u8; 64];
        let mut w = Buf::new(&mut b);
        w.str("!");
        w.hex(0x1a2b3c, 8, false);
        w.byte(b' ');
        w.idec(-45);
        w.byte(b' ');
        w.udec(0);
        w.byte(b' ');
        w.hex(0xAB, 2, true);
        assert_eq!(w.as_bytes(), b"!001a2b3c -45 0 AB");
        let mut tiny = [0u8; 3];
        let mut t = Buf::new(&mut tiny);
        t.str("abcdef");
        assert!(t.overflow && t.as_bytes() == b"abc");
    }
}
