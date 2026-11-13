// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Bit access with the standards' numbering: bit 1 is the first (most significant) bit of the message.

/// Bits `first ..= first + count - 1` (1 based) as a number, `count` <= 32. Bits past the end of `m` read as 0 (a C++ caller would read out of
/// bounds there; the callers of this crate never ask).
pub fn get(m: &[u8], first: i32, count: i32) -> u32 {
    let mut v: u32 = 0;
    for i in 0..count {
        let b = first - 1 + i;
        let bit = if b >= 0 { m.get((b >> 3) as usize).map_or(0, |&x| (x >> (7 - (b & 7))) & 1) } else { 0 };
        v = (v << 1) | bit as u32;
    }
    v
}

/// The same as a signed number in two's complement of `count` bits.
pub fn get_signed(m: &[u8], first: i32, count: i32) -> i32 {
    let mut v = get(m, first, count) as i32;
    if v >= (1 << (count - 1)) {
        v -= 1 << count;
    }
    v
}

/// Writes `value` into bits `first ..` (1 based); bits past the end of `m` are ignored.
pub fn set(m: &mut [u8], first: i32, count: i32, value: u32) {
    for i in 0..count {
        let b = first - 1 + i;
        if b < 0 {
            continue;
        }
        let Some(byte) = m.get_mut((b >> 3) as usize) else { continue };
        let mask = 0x80u8 >> (b & 7);
        if (value >> (count - 1 - i)) & 1 != 0 {
            *byte |= mask;
        } else {
            *byte &= !mask;
        }
    }
}
