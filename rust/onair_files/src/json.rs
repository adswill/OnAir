// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! A small allocation-free JSON reader for SigMF meta files. It accepts exactly what the C++ reader it replaces accepted (see below) and keeps no tree: a value is a
//! `Val` (kind, byte range, number), objects and arrays are walked again where a member is wanted. The whole document is validated first (any structural error
//! anywhere rejects the file), then members are looked up with "the last duplicate key wins".
//!
//! Grammar (identical in `core/src/files/files_proto.cpp`): whitespace is the C `isspace` set; objects, arrays, `true` / `false` / `null`, strings, numbers; at most 64
//! levels deep; text after the top-level value is ignored. Strings may hold raw control characters; escapes: `\n \t \r \uXXXX` (a code point under 128 stays,
//! anything else becomes `?`, and `\u` needs four more characters to exist or it is dropped) and `\x` = `x` for every other `x` (so `\"` `\\` `\/`). Numbers are
//! a decimal token, `[+-]? (digits [. digits*] | . digits) ([eE] [+-]? digits)?`, converted like `strtod` (correctly rounded); hex floats, `inf` and `nan` are not numbers.
use core::str::FromStr;

/// Deepest nesting.
const MAX_DEPTH: i32 = 64;

/// What a value is.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    /// `null`, or a value that failed to parse.
    Null,
    /// A number (see `Val::num`).
    Num,
    /// A string (range = the bytes between the quotes, escapes not resolved).
    Str,
    /// `true` or `false`.
    Bool,
    /// An array (range includes the brackets).
    Arr,
    /// An object (range includes the braces).
    Obj,
}

/// A parsed value: its kind, where it sits in the text and, for numbers, its value.
#[derive(Clone, Copy, Debug)]
pub struct Val {
    /// Kind of value.
    pub kind: Kind,
    /// First byte (for a string: after the opening quote).
    pub start: usize,
    /// One past the last byte (for a string: the closing quote).
    pub end: usize,
    /// Value of a number, 0 otherwise.
    pub num: f64,
}

impl Val {
    const NULL: Val = Val { kind: Kind::Null, start: 0, end: 0, num: 0.0 };
}

/// The reader over a text.
pub struct Reader<'a> {
    s: &'a [u8],
    i: usize,
    /// Set by any structural error.
    pub bad: bool,
    depth: i32,
}

/// C `isspace` in the C locale.
fn is_space(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\n' | 0x0B | 0x0C | b'\r')
}

impl<'a> Reader<'a> {
    /// A reader at the start of `s`.
    pub fn new(s: &'a [u8]) -> Reader<'a> {
        Reader { s, i: 0, bad: false, depth: 0 }
    }

    /// A reader starting at byte `at` (for walking into an object or array found earlier).
    pub fn at(s: &'a [u8], at: usize) -> Reader<'a> {
        Reader { s, i: at, bad: false, depth: 0 }
    }

    fn ws(&mut self) {
        while self.i < self.s.len() && is_space(self.s[self.i]) {
            self.i += 1;
        }
    }

    fn peek(&self) -> Option<u8> {
        self.s.get(self.i).copied()
    }

    fn lit(&mut self, w: &[u8]) -> bool {
        if self.s.len() >= self.i + w.len() && &self.s[self.i..self.i + w.len()] == w {
            self.i += w.len();
            true
        } else {
            false
        }
    }

    /// Reads a string whose opening quote is at `i`; returns the range between the quotes. Sets `bad` when it is not terminated.
    fn string(&mut self) -> (usize, usize) {
        self.i += 1;
        let start = self.i;
        while self.i < self.s.len() && self.s[self.i] != b'"' {
            if self.s[self.i] == b'\\' && self.i + 1 < self.s.len() {
                self.i += 1;
                if self.s[self.i] == b'u' && self.i + 4 < self.s.len() {
                    self.i += 4;
                }
            }
            self.i += 1;
        }
        let end = self.i.min(self.s.len());
        if self.i >= self.s.len() {
            self.bad = true;
        } else {
            self.i += 1;
        }
        (start, end)
    }

    /// The number token at `i` or None.
    fn number(&mut self) -> Option<f64> {
        let s = self.s;
        let mut j = self.i;
        let st = j;
        if matches!(s.get(j), Some(b'+') | Some(b'-')) {
            j += 1;
        }
        let d0 = j;
        while matches!(s.get(j), Some(b'0'..=b'9')) {
            j += 1;
        }
        let int_digits = j - d0;
        let mut frac_digits = 0;
        if s.get(j) == Some(&b'.') {
            let f0 = j + 1;
            let mut k = f0;
            while matches!(s.get(k), Some(b'0'..=b'9')) {
                k += 1;
            }
            frac_digits = k - f0;
            if int_digits > 0 || frac_digits > 0 {
                j = k;
            }
        }
        if int_digits == 0 && frac_digits == 0 {
            return None;
        }
        if matches!(s.get(j), Some(b'e') | Some(b'E')) {
            let mut k = j + 1;
            if matches!(s.get(k), Some(b'+') | Some(b'-')) {
                k += 1;
            }
            let e0 = k;
            while matches!(s.get(k), Some(b'0'..=b'9')) {
                k += 1;
            }
            if k > e0 {
                j = k;
            }
        }
        let tok = &s[st..j];
        self.i = j;
        Some(parse_f64(tok))
    }

    /// Parses the value at the current position.
    pub fn val(&mut self) -> Val {
        self.ws();
        self.depth += 1;
        if self.i >= self.s.len() || self.depth > MAX_DEPTH {
            self.bad = true;
            return Val::NULL;
        }
        let c = self.s[self.i];
        let start = self.i;
        let mut v = Val::NULL;
        if c == b'{' {
            v.kind = Kind::Obj;
            self.i += 1;
            self.ws();
            if self.peek() == Some(b'}') {
                self.i += 1;
            } else {
                loop {
                    self.ws();
                    if self.peek() != Some(b'"') {
                        self.bad = true;
                        break;
                    }
                    self.string();
                    if self.bad {
                        break;
                    }
                    self.ws();
                    if self.peek() != Some(b':') {
                        self.bad = true;
                        break;
                    }
                    self.i += 1;
                    self.val();
                    if self.bad {
                        break;
                    }
                    self.ws();
                    match self.peek() {
                        Some(b',') => {
                            self.i += 1;
                        }
                        Some(b'}') => {
                            self.i += 1;
                            break;
                        }
                        _ => {
                            self.bad = true;
                            break;
                        }
                    }
                }
            }
        } else if c == b'[' {
            v.kind = Kind::Arr;
            self.i += 1;
            self.ws();
            if self.peek() == Some(b']') {
                self.i += 1;
            } else {
                loop {
                    self.val();
                    if self.bad {
                        break;
                    }
                    self.ws();
                    match self.peek() {
                        Some(b',') => {
                            self.i += 1;
                        }
                        Some(b']') => {
                            self.i += 1;
                            break;
                        }
                        _ => {
                            self.bad = true;
                            break;
                        }
                    }
                }
            }
        } else if c == b'"' {
            v.kind = Kind::Str;
            let (a, b) = self.string();
            v.start = a;
            v.end = b;
            self.depth -= 1;
            return v;
        } else if self.lit(b"true") || self.lit(b"false") {
            v.kind = Kind::Bool;
        } else if self.lit(b"null") {
        } else {
            match self.number() {
                Some(x) => {
                    v.kind = Kind::Num;
                    v.num = x;
                }
                None => self.bad = true,
            }
        }
        v.start = start;
        v.end = self.i.min(self.s.len());
        self.depth -= 1;
        v
    }
}

/// Converts a validated decimal token like `strtod` (correctly rounded, any length; overflow gives infinity).
pub fn parse_f64(tok: &[u8]) -> f64 {
    match core::str::from_utf8(tok) {
        Ok(t) => f64::from_str(t).unwrap_or(0.0),
        Err(_) => 0.0,
    }
}

/// Resolves the escapes of a raw string range into `out` (see the module comment); returns the number of bytes the result has (it never grows beyond the raw
/// length). Writes at most `out.len()` bytes but always returns the full length.
pub fn unescape(raw: &[u8], out: &mut [u8]) -> usize {
    let mut n = 0usize;
    let mut put = |b: u8, n: &mut usize| {
        if *n < out.len() {
            out[*n] = b;
        }
        *n += 1;
    };
    let mut i = 0usize;
    while i < raw.len() {
        if raw[i] == b'\\' && i + 1 < raw.len() {
            i += 1;
            match raw[i] {
                b'n' => put(b'\n', &mut n),
                b't' => put(b'\t', &mut n),
                b'r' => put(b'\r', &mut n),
                b'u' => {
                    if i + 4 < raw.len() {
                        let c = hex4(&raw[i + 1..i + 5]);
                        put(if c < 128 { c as u8 } else { b'?' }, &mut n);
                        i += 4;
                    }
                }
                c => put(c, &mut n),
            }
        } else {
            put(raw[i], &mut n);
        }
        i += 1;
    }
    n
}

/// `strtoul(.., 16)` of up to four characters: leading blanks, an optional sign and `0x` prefix are accepted, parsing stops at the first non-hex character.
fn hex4(b: &[u8]) -> u32 {
    let mut i = 0;
    while i < b.len() && is_space(b[i]) {
        i += 1;
    }
    let mut neg = false;
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        neg = b[i] == b'-';
        i += 1;
    }
    if i + 2 < b.len() && b[i] == b'0' && (b[i + 1] == b'x' || b[i + 1] == b'X') && b[i + 2].is_ascii_hexdigit() {
        i += 2;
    }
    let mut v: u32 = 0;
    while i < b.len() && b[i].is_ascii_hexdigit() {
        v = v * 16 + (b[i] as char).to_digit(16).unwrap_or(0);
        i += 1;
    }
    if neg {
        v.wrapping_neg()
    } else {
        v
    }
}

/// True when the string range `raw` unescapes to exactly `key`.
pub fn key_is(raw: &[u8], key: &[u8]) -> bool {
    if raw.len() < key.len() {
        return false; // unescaping never lengthens
    }
    let mut buf = [0u8; 64];
    if key.len() >= buf.len() {
        return false;
    }
    // compare against a bounded unescape of the first key.len()+1 output bytes
    let n = unescape(raw, &mut buf[..key.len() + 1]);
    n == key.len() && buf[..key.len()] == *key
}

/// The last member called `key` of the object `obj` (a `Val` of kind Obj). `Val::NULL`-like (kind Null, start == end == 0) when there is none or `obj` is not an object.
pub fn get(s: &[u8], obj: &Val, key: &[u8]) -> Option<Val> {
    if obj.kind != Kind::Obj {
        return None;
    }
    let mut r = Reader::at(s, obj.start + 1);
    r.ws();
    if r.peek() == Some(b'}') {
        return None;
    }
    let mut found = None;
    loop {
        r.ws();
        if r.peek() != Some(b'"') {
            break;
        }
        let (a, b) = r.string();
        if r.bad {
            break;
        }
        r.ws();
        if r.peek() != Some(b':') {
            break;
        }
        r.i += 1;
        let v = r.val();
        if r.bad {
            break;
        }
        if key_is(&s[a..b], key) {
            found = Some(v);
        }
        r.ws();
        if r.peek() == Some(b',') {
            r.i += 1;
            continue;
        }
        break;
    }
    found
}

/// The `idx`-th element of the array `arr` and the array's length, as (element, length). The element is None past the end.
pub fn elem(s: &[u8], arr: &Val, idx: usize) -> (Option<Val>, usize) {
    if arr.kind != Kind::Arr {
        return (None, 0);
    }
    let mut r = Reader::at(s, arr.start + 1);
    r.ws();
    if r.peek() == Some(b']') {
        return (None, 0);
    }
    let mut n = 0usize;
    let mut got = None;
    loop {
        let v = r.val();
        if r.bad {
            break;
        }
        if n == idx {
            got = Some(v);
        }
        n += 1;
        r.ws();
        if r.peek() == Some(b',') {
            r.i += 1;
            continue;
        }
        break;
    }
    (got, n)
}

/// Walks the array `arr` once, calling `f(index, element)` for every element; returns the element count (0 when `arr` is not an array).
pub fn each_elem(s: &[u8], arr: &Val, mut f: impl FnMut(usize, &Val)) -> usize {
    if arr.kind != Kind::Arr {
        return 0;
    }
    let mut r = Reader::at(s, arr.start + 1);
    r.ws();
    if r.peek() == Some(b']') {
        return 0;
    }
    let mut n = 0usize;
    loop {
        let v = r.val();
        if r.bad {
            break;
        }
        f(n, &v);
        n += 1;
        r.ws();
        if r.peek() == Some(b',') {
            r.i += 1;
            continue;
        }
        break;
    }
    n
}
