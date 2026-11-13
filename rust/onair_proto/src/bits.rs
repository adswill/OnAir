// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! AIS bit fields. Bits are one per byte in transmission order; only bit 0 of each byte counts (as in the C++ code).

pub use onair_common::bits::unpacked::{get_s, get_u};

/// 6-bit text into `out`; trailing '@' and spaces removed. Returns the length (at most `len / 6`, and at most `out.len()`).
pub fn get_text(b: &[u8], pos: usize, len: usize, out: &mut [u8]) -> usize {
    let mut n = 0;
    let mut i = 0;
    while i + 6 <= len && n < out.len() {
        let v = get_u(b, pos.wrapping_add(i), 6);
        out[n] = if v < 32 { (v + 64) as u8 } else { v as u8 };
        n += 1;
        i += 6;
    }
    while n > 0 && (out[n - 1] == b'@' || out[n - 1] == b' ') {
        n -= 1;
    }
    n
}
