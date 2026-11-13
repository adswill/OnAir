// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The request bodies of the remote-control API: a flat JSON object (`{"a": 1, "b": "x", "c": true, "d": null}`), no nesting, no arrays, at most 32 keys.
//! Strings are unescaped (`\uXXXX` becomes UTF-8, a lone surrogate is encoded as is) into the caller's arena; the result is a list of entries that point into it.
//! A duplicate key replaces the earlier value. Anything malformed gives `None`.
use core::str::FromStr;

/// Most keys an object may have.
pub const MAX_KEYS: usize = 32;

/// Kinds of value.
pub const NULL: u32 = 0;
/// A number.
pub const NUM: u32 = 1;
/// A string.
pub const STR: u32 = 2;
/// A boolean.
pub const BOOL: u32 = 3;

/// One member of the object: `key` and (for strings) `str` are ranges of the arena the parser was given.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq)]
pub struct Entry {
    /// Kind of value (`NULL`, `NUM`, `STR`, `BOOL`).
    pub kind: u32,
    /// Boolean value (0 / 1).
    pub b: u32,
    /// Numeric value.
    pub num: f64,
    /// Key bytes in the arena.
    pub key_off: u32,
    /// Key length.
    pub key_len: u32,
    /// String bytes in the arena.
    pub str_off: u32,
    /// String length.
    pub str_len: u32,
}

struct P<'a> {
    t: &'a [u8],
    i: usize,
    arena: &'a mut [u8],
    used: usize,
}

impl P<'_> {
    fn ws(&mut self) {
        while self.i < self.t.len() && matches!(self.t[self.i], b' ' | b'\t' | b'\r' | b'\n') {
            self.i += 1;
        }
    }

    fn push(&mut self, b: u8) -> bool {
        if self.used >= self.arena.len() {
            return false;
        }
        self.arena[self.used] = b;
        self.used += 1;
        true
    }

    /// A string at `i` (opening quote): the unescaped bytes are appended to the arena; returns (offset, length).
    fn string(&mut self) -> Option<(usize, usize)> {
        if self.i >= self.t.len() || self.t[self.i] != b'"' {
            return None;
        }
        self.i += 1;
        let start = self.used;
        while self.i < self.t.len() && self.t[self.i] != b'"' {
            let c = self.t[self.i];
            self.i += 1;
            if c < 0x20 {
                return None;
            }
            if c == b'\\' {
                if self.i >= self.t.len() {
                    return None;
                }
                let e = self.t[self.i];
                self.i += 1;
                match e {
                    b'"' | b'\\' | b'/' => {
                        self.push(e);
                    }
                    b'b' => {
                        self.push(8);
                    }
                    b'f' => {
                        self.push(12);
                    }
                    b'n' => {
                        self.push(b'\n');
                    }
                    b'r' => {
                        self.push(b'\r');
                    }
                    b't' => {
                        self.push(b'\t');
                    }
                    b'u' => {
                        if self.i + 4 > self.t.len() {
                            return None;
                        }
                        let mut v: u32 = 0;
                        for _ in 0..4 {
                            let h = self.t[self.i];
                            self.i += 1;
                            v = v * 16 + (h as char).to_digit(16)?;
                        }
                        if v < 0x80 {
                            self.push(v as u8);
                        } else if v < 0x800 {
                            self.push(0xC0 | (v >> 6) as u8);
                            self.push(0x80 | (v & 63) as u8);
                        } else {
                            self.push(0xE0 | (v >> 12) as u8);
                            self.push(0x80 | ((v >> 6) & 63) as u8);
                            self.push(0x80 | (v & 63) as u8);
                        }
                    }
                    _ => return None,
                }
            } else {
                self.push(c);
            }
        }
        if self.i >= self.t.len() {
            return None;
        }
        self.i += 1;
        Some((start, self.used - start))
    }
}

fn key_eq(arena: &[u8], e: &Entry, off: usize, len: usize) -> bool {
    e.key_len as usize == len && arena[e.key_off as usize..e.key_off as usize + len] == arena[off..off + len]
}

/// Parses `t`. `entries` (at most `MAX_KEYS`) and the arena (at least `t.len()` bytes: unescaping never lengthens) receive the result; returns the entry count, or
/// None for anything that is not a flat JSON object, has more than 32 different keys, a number that is not finite, or text after the object.
pub fn parse(t: &[u8], entries: &mut [Entry], arena: &mut [u8]) -> Option<usize> {
    let mut p = P { t, i: 0, arena, used: 0 };
    let mut n = 0usize;
    p.ws();
    if p.i >= t.len() || t[p.i] != b'{' {
        return None;
    }
    p.i += 1;
    p.ws();
    if p.i < t.len() && t[p.i] == b'}' {
        p.i += 1;
        p.ws();
        return if p.i == t.len() { Some(0) } else { None };
    }
    loop {
        p.ws();
        let (ko, kl) = p.string()?;
        p.ws();
        if p.i >= t.len() || t[p.i] != b':' {
            return None;
        }
        p.i += 1;
        p.ws();
        let mut e = Entry { key_off: ko as u32, key_len: kl as u32, ..Entry::default() };
        if p.i < t.len() && t[p.i] == b'"' {
            e.kind = STR;
            let (so, sl) = p.string()?;
            e.str_off = so as u32;
            e.str_len = sl as u32;
        } else if t[p.i..].starts_with(b"true") {
            e.kind = BOOL;
            e.b = 1;
            p.i += 4;
        } else if t[p.i..].starts_with(b"false") {
            e.kind = BOOL;
            p.i += 5;
        } else if t[p.i..].starts_with(b"null") {
            e.kind = NULL;
            p.i += 4;
        } else {
            let mut j = p.i;
            while j < t.len() && (t[j].is_ascii_digit() || matches!(t[j], b'-' | b'+' | b'.' | b'e' | b'E')) {
                j += 1;
            }
            if j == p.i || j - p.i > 32 {
                return None;
            }
            let tok = &t[p.i..j];
            // the whole token must be one decimal number (the C library would also stop at a lone sign or a second dot)
            if decimal_len(tok) != tok.len() {
                return None;
            }
            let v = f64::from_str(core::str::from_utf8(tok).ok()?).ok()?;
            if !v.is_finite() {
                return None;
            }
            e.kind = NUM;
            e.num = v;
            p.i = j;
        }
        // a new key past the 32nd is refused; an existing key is overwritten
        let dup = (0..n).find(|&k| key_eq(p.arena, &entries[k], ko, kl));
        match dup {
            Some(k) => entries[k] = e,
            None => {
                if n >= MAX_KEYS || n >= entries.len() {
                    return None;
                }
                entries[n] = e;
                n += 1;
            }
        }
        p.ws();
        if p.i < t.len() && t[p.i] == b',' {
            p.i += 1;
            continue;
        }
        if p.i < t.len() && t[p.i] == b'}' {
            p.i += 1;
            break;
        }
        return None;
    }
    p.ws();
    if p.i == t.len() {
        Some(n)
    } else {
        None
    }
}

/// Length of the decimal number at the start of `t` (`[+-]? (digits [. digits*] | . digits) ([eE] [+-]? digits)?`), 0 if none. The same grammar `strtod` follows
/// for the characters a number token may hold here.
fn decimal_len(t: &[u8]) -> usize {
    let mut j = 0;
    if matches!(t.first(), Some(b'+') | Some(b'-')) {
        j += 1;
    }
    let d0 = j;
    while matches!(t.get(j), Some(b'0'..=b'9')) {
        j += 1;
    }
    let int_digits = j - d0;
    let mut frac = 0;
    if t.get(j) == Some(&b'.') {
        let mut k = j + 1;
        while matches!(t.get(k), Some(b'0'..=b'9')) {
            k += 1;
        }
        frac = k - j - 1;
        if int_digits > 0 || frac > 0 {
            j = k;
        }
    }
    if int_digits == 0 && frac == 0 {
        return 0;
    }
    if matches!(t.get(j), Some(b'e') | Some(b'E')) {
        let mut k = j + 1;
        if matches!(t.get(k), Some(b'+') | Some(b'-')) {
            k += 1;
        }
        let e0 = k;
        while matches!(t.get(k), Some(b'0'..=b'9')) {
            k += 1;
        }
        if k > e0 {
            j = k;
        }
    }
    j
}
