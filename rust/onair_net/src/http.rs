// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The head of an HTTP/1.1 request as the remote-control API reads it (RFC 9112 section 3): request line, header fields, and the checks that decide whether a body
//! may follow. Works on the bytes the caller has gathered; nothing is allocated, the results are ranges of the input.
use crate::Span;

/// Longest request head (request line and header fields) in bytes.
pub const MAX_HEADER: usize = 8192;
/// Largest request body.
pub const MAX_BODY: usize = 4096;
/// Most header fields a request may carry.
pub const MAX_HEADERS: usize = 48;
/// Longest request target.
pub const MAX_TARGET: usize = 512;

/// `scan`: the head is not complete yet.
pub const SCAN_NEED: u32 = 0;
/// `scan`: the empty line ending the head is in the buffer.
pub const SCAN_FOUND: u32 = 1;
/// `scan`: the head is longer than `MAX_HEADER` (answer 431).
pub const SCAN_TOO_BIG: u32 = 2;

/// Looks for the end of the head in what has arrived: (state, offset of the CR LF CR LF that ends it).
pub fn scan(buf: &[u8]) -> (u32, usize) {
    match buf.windows(4).position(|w| w == b"\r\n\r\n") {
        Some(p) if p > MAX_HEADER => (SCAN_TOO_BIG, p),
        Some(p) => (SCAN_FOUND, p),
        None if buf.len() > MAX_HEADER => (SCAN_TOO_BIG, 0),
        None => (SCAN_NEED, 0),
    }
}

/// No error.
pub const OK: u32 = 0;
/// 400 "bad request line"
pub const E_REQUEST_LINE: u32 = 1;
/// 400 "bad target"
pub const E_TARGET: u32 = 2;
/// 400 "bad header"
pub const E_HEADER: u32 = 3;
/// 400 "duplicate header"
pub const E_DUPLICATE: u32 = 4;
/// 501 "chunked bodies are not supported"
pub const E_CHUNKED: u32 = 5;
/// 400 "bad Content-Length"
pub const E_CONTENT_LENGTH: u32 = 6;
/// 413 "body too large (limit 4096 bytes)"
pub const E_BODY_TOO_LARGE: u32 = 7;
/// 411 "Content-Length required"
pub const E_LENGTH_REQUIRED: u32 = 8;

/// The HTTP status and the message text of an error id (0, "" for `OK` and unknown ids).
pub fn error(id: u32) -> (u32, &'static str) {
    match id {
        E_REQUEST_LINE => (400, "bad request line"),
        E_TARGET => (400, "bad target"),
        E_HEADER => (400, "bad header"),
        E_DUPLICATE => (400, "duplicate header"),
        E_CHUNKED => (501, "chunked bodies are not supported"),
        E_CONTENT_LENGTH => (400, "bad Content-Length"),
        E_BODY_TOO_LARGE => (413, "body too large (limit 4096 bytes)"),
        E_LENGTH_REQUIRED => (411, "Content-Length required"),
        _ => (0, ""),
    }
}

/// One header field: the name as sent (trimmed, the caller lower-cases it) and the value (trimmed).
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Field {
    /// Name range.
    pub name: Span,
    /// Value range.
    pub value: Span,
}

/// What `parse` found.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Head {
    /// `OK` or an error id (see `error`).
    pub err: u32,
    /// Header fields written to the caller's array.
    pub n_fields: u32,
    /// Declared body length (0 when there is no Content-Length).
    pub content_length: u32,
    /// Method (everything before the first space; may be empty).
    pub method: Span,
    /// Request target up to the `?`.
    pub path: Span,
    /// Request target after the `?` (empty without one).
    pub query: Span,
}

fn trim(s: &[u8], off: usize) -> Span {
    let (mut a, mut b) = (0, s.len());
    while a < b && (s[a] == b' ' || s[a] == b'\t') {
        a += 1;
    }
    while b > a && matches!(s[b - 1], b' ' | b'\t' | b'\r') {
        b -= 1;
    }
    Span { off: (off + a) as u32, len: (b - a) as u32 }
}

fn find(h: &[u8], from: usize, pat: &[u8]) -> Option<usize> {
    if from > h.len() {
        return None;
    }
    h[from..].windows(pat.len()).position(|w| w == pat).map(|p| p + from)
}

fn fail(err: u32) -> Head {
    Head { err, ..Head::default() }
}

fn name_is(name: &[u8], lower: &[u8]) -> bool {
    name.len() == lower.len() && name.iter().zip(lower).all(|(a, b)| a.to_ascii_lowercase() == *b)
}

/// Parses the head (everything before the empty line, without it). Header fields go to `fields` (`MAX_HEADERS` of them fit; a request with more is refused).
/// `post` is whether the method is exactly POST (a POST needs a Content-Length).
pub fn parse(head: &[u8], fields: &mut [Field]) -> Head {
    let line_end = find(head, 0, b"\r\n");
    let line = &head[..line_end.unwrap_or(head.len())];
    let s1 = match line.iter().position(|&c| c == b' ') {
        Some(p) => p,
        None => return fail(E_REQUEST_LINE),
    };
    let s2 = line.iter().rposition(|&c| c == b' ').unwrap_or(s1);
    if s2 == s1 || !line[s2 + 1..].starts_with(b"HTTP/") {
        return fail(E_REQUEST_LINE);
    }
    let target = &line[s1 + 1..s2];
    if target.is_empty() || target[0] != b'/' || target.len() > MAX_TARGET || target.iter().any(|&c| c < 0x21 || c == 0x7f) {
        return fail(E_TARGET);
    }
    let q = target.iter().position(|&c| c == b'?');
    let base = s1 + 1;
    let mut h = Head {
        method: Span { off: 0, len: s1 as u32 },
        path: Span { off: base as u32, len: q.unwrap_or(target.len()) as u32 },
        query: match q {
            Some(p) => Span { off: (base + p + 1) as u32, len: (target.len() - p - 1) as u32 },
            None => Span { off: (base + target.len()) as u32, len: 0 },
        },
        ..Head::default()
    };
    let (mut seen_cl, mut seen_host, mut seen_te) = (None::<Span>, false, false);
    let mut nh = 0usize;
    let mut pos = line_end;
    while let Some(p) = pos {
        if p >= head.len() {
            break;
        }
        let nx = find(head, p + 2, b"\r\n");
        let l_off = p + 2;
        let l = &head[l_off..nx.unwrap_or(head.len())];
        pos = nx;
        if l.is_empty() {
            continue;
        }
        let c = match l.iter().position(|&c| c == b':') {
            Some(c) if c != 0 => c,
            _ => return fail(E_HEADER),
        };
        nh += 1;
        if nh > MAX_HEADERS || nh > fields.len() {
            return fail(E_HEADER);
        }
        let name = trim(&l[..c], l_off);
        let value = trim(&l[c + 1..], l_off + c + 1);
        let nm = &head[name.off as usize..(name.off + name.len) as usize];
        let is_cl = name_is(nm, b"content-length");
        let is_host = name_is(nm, b"host");
        if (is_cl && seen_cl.is_some()) || (is_host && seen_host) {
            return fail(E_DUPLICATE);
        }
        if is_cl {
            seen_cl = Some(value);
        }
        seen_host |= is_host;
        seen_te |= name_is(nm, b"transfer-encoding");
        fields[nh - 1] = Field { name, value };
    }
    h.n_fields = nh as u32;
    if seen_te {
        return Head { err: E_CHUNKED, ..h };
    }
    if let Some(v) = seen_cl {
        let d = &head[v.off as usize..(v.off + v.len) as usize];
        if d.is_empty() || d.len() > 9 || !d.iter().all(u8::is_ascii_digit) {
            return Head { err: E_CONTENT_LENGTH, ..h };
        }
        let want = d.iter().fold(0u32, |a, &c| a * 10 + (c - b'0') as u32);
        if want as usize > MAX_BODY {
            return Head { err: E_BODY_TOO_LARGE, ..h };
        }
        h.content_length = want;
    } else if &head[..s1] == b"POST" {
        return Head { err: E_LENGTH_REQUIRED, ..h };
    }
    h
}
