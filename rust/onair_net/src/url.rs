// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Small text helpers of the HTTP side of the remote-control API: percent decoding, the query string, the Host header, loopback names, the constant time compare
//! of the token, reason phrases.

/// Value of a hex digit.
fn hexv(c: u8) -> Option<u8> {
    (c as char).to_digit(16).map(|d| d as u8)
}

/// Walks the decoded bytes of `s` (`%XX` and `+`; a `%` without two hex digits after it is kept, and a `%` in the last two bytes never starts an escape: the test is
/// `i + 2 < len`, like the reference), calling `f` for each; stops early when `f` returns false.
fn decode_each(s: &[u8], mut f: impl FnMut(u8) -> bool) {
    let mut i = 0;
    while i < s.len() {
        let c = s[i];
        let v = if c == b'%' && i + 2 < s.len() {
            match (hexv(s[i + 1]), hexv(s[i + 2])) {
                (Some(a), Some(b)) => Some(a * 16 + b),
                _ => None,
            }
        } else {
            None
        };
        let byte = if let Some(b) = v {
            i += 2;
            b
        } else if c == b'+' {
            b' '
        } else {
            c
        };
        if !f(byte) {
            return;
        }
        i += 1;
    }
}

/// Decodes `s` into `out` (which needs `s.len()` bytes at most); returns the decoded length (also when `out` was too short; only what fits is written).
pub fn url_decode(s: &[u8], out: &mut [u8]) -> usize {
    let mut n = 0;
    decode_each(s, |b| {
        if n < out.len() {
            out[n] = b;
        }
        n += 1;
        true
    });
    n
}

/// The decoded value of query parameter `key` in `q` ("a=1&b=2"): the first parameter whose decoded name equals `key`; a name without `=` has an empty value.
/// Returns (found, length of the value); the value is written to `out` (decoded, cut to its length).
pub fn query_param(q: &[u8], key: &[u8], out: &mut [u8]) -> (bool, usize) {
    let mut i = 0usize;
    while i <= q.len() {
        let mut e = i;
        while e < q.len() && q[e] != b'&' {
            e += 1;
        }
        let kv = &q[i..e];
        let eq = kv.iter().position(|&c| c == b'=');
        let name = &kv[..eq.unwrap_or(kv.len())];
        if decoded_equals(name, key) {
            return match eq {
                None => (true, 0),
                Some(p) => (true, url_decode(&kv[p + 1..], out)),
            };
        }
        i = e + 1;
    }
    (false, 0)
}

/// True when the decoded `a` is exactly `b`.
fn decoded_equals(a: &[u8], b: &[u8]) -> bool {
    let mut k = 0usize;
    let mut same = true;
    decode_each(a, |c| {
        if k >= b.len() || b[k] != c {
            same = false;
            return false;
        }
        k += 1;
        true
    });
    same && k == b.len()
}

/// "127.0.0.1:8765" -> "127.0.0.1", "[::1]:80" -> "[::1]": the end of the part that names the host.
pub fn host_only_len(h: &[u8]) -> usize {
    if !h.is_empty() && h[0] == b'[' {
        return match h.iter().position(|&c| c == b']') {
            Some(e) => e + 1,
            None => h.len(),
        };
    }
    match h.iter().rposition(|&c| c == b':') {
        Some(c) => c,
        None => h.len(),
    }
}

/// `localhost`, `[::1]` or a dotted `127.x.y.z`.
pub fn loopback_name(n: &[u8]) -> bool {
    if n == b"localhost" || n == b"[::1]" {
        return true;
    }
    n.starts_with(b"127.") && n.iter().all(|&c| c.is_ascii_digit() || c == b'.')
}

/// Constant time equality (the time does not depend on where the texts differ; different lengths are unequal).
pub fn ct_equal(a: &[u8], b: &[u8]) -> bool {
    let mut d: u8 = if a.len() == b.len() { 0 } else { 1 };
    for i in 0..a.len().min(b.len()) {
        d |= a[i] ^ b[i];
    }
    d == 0
}

/// The reason phrase of an HTTP status code the API uses.
pub fn status_text(c: i32) -> &'static str {
    match c {
        200 => "OK",
        101 => "Switching Protocols",
        400 => "Bad Request",
        401 => "Unauthorized",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        408 => "Request Timeout",
        411 => "Length Required",
        413 => "Payload Too Large",
        415 => "Unsupported Media Type",
        426 => "Upgrade Required",
        431 => "Request Header Fields Too Large",
        501 => "Not Implemented",
        503 => "Service Unavailable",
        _ => "Error",
    }
}
