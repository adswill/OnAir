// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The rtl_tcp protocol (librtlsdr's `rtl_tcp.c`): the 12 byte greeting ("RTL0", tuner type, number of gains, the numbers big endian), the 5 byte commands (one command
//! byte and a big endian 32-bit parameter), the gain tables of the tuners, and the `host:port` address form shared with the SpyServer and SDR++ clients.
use crate::{atoi, Span};

/// Tuner types of the greeting.
pub const E4000: u32 = 1;
/// FC0012.
pub const FC0012: u32 = 2;
/// FC0013.
pub const FC0013: u32 = 3;
/// FC2580.
pub const FC2580: u32 = 4;
/// R820T.
pub const R820T: u32 = 5;
/// R828D.
pub const R828D: u32 = 6;

/// The greeting a server sends on connect.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Greeting {
    /// The four characters ("RTL0" for a real rtl_tcp).
    pub magic: [u8; 4],
    /// Tuner type.
    pub tuner: u32,
    /// Number of gains the server lists.
    pub n_gains: u32,
    /// 1 when this is not an RTL-SDR (another magic or a tuner type outside 1 to 6).
    pub other: u32,
}

/// Reads the 12 byte greeting: None when it is shorter, a magic that is not printable ASCII, or a tuner type / gain count over 0xffff (not an rtl_tcp server).
pub fn greeting(h: &[u8]) -> Option<Greeting> {
    if h.len() < 12 {
        return None;
    }
    if h[..4].iter().any(|&c| !(0x20..=0x7e).contains(&c)) {
        return None;
    }
    let tuner = u32::from_be_bytes([h[4], h[5], h[6], h[7]]);
    let n_gains = u32::from_be_bytes([h[8], h[9], h[10], h[11]]);
    if tuner > 0xffff || n_gains > 0xffff {
        return None;
    }
    let magic = [h[0], h[1], h[2], h[3]];
    Some(Greeting { magic, tuner, n_gains, other: (&magic != b"RTL0" || !(E4000..=R828D).contains(&tuner)) as u32 })
}

/// The gains (tenths of a dB) of a tuner as librtlsdr's drivers list them; an unknown tuner gets the R820T's.
pub fn tuner_gains(tuner: i32) -> &'static [i16] {
    match tuner {
        1 => &[-10, 15, 40, 65, 90, 115, 140, 165, 190, 215, 240, 290, 340, 420],
        2 => &[-99, -40, 71, 179, 192],
        3 => &[-99, -73, -65, -63, -60, -58, -54, 58, 61, 63, 65, 67, 68, 70, 71, 179, 181, 182, 184, 186, 188, 191, 197],
        4 => &[0],
        _ => &[
            0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445,
            480, 496,
        ],
    }
}

/// The tuner's name.
pub fn tuner_name(t: i32) -> &'static str {
    match t {
        1 => "E4000",
        2 => "FC0012",
        3 => "FC0013",
        4 => "FC2580",
        5 => "R820T",
        6 => "R828D",
        _ => "unknown tuner",
    }
}

/// A command: the command byte and the parameter, big endian.
pub fn command(cmd: u8, v: u32) -> [u8; 5] {
    let b = v.to_be_bytes();
    [cmd, b[0], b[1], b[2], b[3]]
}

/// Splits "host:port" ("[::1]:1234" gives the host without brackets) into the host range and the port; None when there is no host or the port is not 1 to 65535.
/// The port is read like the C library's `atoi`.
pub fn split_host_port(a: &[u8]) -> Option<(Span, i32)> {
    let c = a.iter().rposition(|&x| x == b':')?;
    if c == 0 {
        return None;
    }
    let (mut off, mut len) = (0usize, c);
    if len > 2 && a[0] == b'[' && a[c - 1] == b']' {
        off = 1;
        len = c - 2;
    }
    let port = atoi(&a[c + 1..]);
    if port > 0 && port < 65536 {
        Some((Span { off: off as u32, len: len as u32 }, port))
    } else {
        None
    }
}
