// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! OnAir network: the pure protocol layers of the network code, over byte buffers (no sockets, no threads, no allocation): the remote-control API's HTTP and WebSocket
//! side (SHA-1 / base64 for the handshake, frames, request head, URL and query text, the flat JSON bodies), the rtl_tcp, SpyServer and SDR++ server protocols, and the
//! list of network radios the settings keep. Sockets, connections, state and the radio drivers stay in C++. Safe code goes in the modules next to `ffi`; only `ffi` may
//! use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod flatjson;
pub mod hash;
pub mod http;
pub mod radios;
pub mod rtltcp;
pub mod sdrpp;
pub mod spy;
pub mod url;
pub mod ws;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;

/// A byte range of a text the caller gave (text fields come back as ranges; the caller copies them out).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Span {
    /// First byte.
    pub off: u32,
    /// Number of bytes.
    pub len: u32,
}

/// The C library's `atoi` on a C string whose bytes are `s` (it ends at the first NUL or at the end of `s`): leading white space, an optional sign, decimal digits; a
/// value that does not fit a `long` (64 bits on the platforms OnAir runs C++ reference tests on) clamps like `strtol`, then the cast to `int` wraps like the C one.
pub fn atoi(s: &[u8]) -> i32 {
    let s = &s[..s.iter().position(|&c| c == 0).unwrap_or(s.len())];
    let mut i = 0;
    while i < s.len() && matches!(s[i], b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r') {
        i += 1;
    }
    let neg = i < s.len() && s[i] == b'-';
    if i < s.len() && (s[i] == b'-' || s[i] == b'+') {
        i += 1;
    }
    let mut v: i64 = 0;
    let mut over = false;
    while i < s.len() && s[i].is_ascii_digit() {
        let d = (s[i] - b'0') as i64;
        match v.checked_mul(10).and_then(|x| if neg { x.checked_sub(d) } else { x.checked_add(d) }) {
            Some(x) => v = x,
            None => over = true,
        }
        i += 1;
    }
    if over {
        v = if neg { i64::MIN } else { i64::MAX };
    }
    v as i32
}
