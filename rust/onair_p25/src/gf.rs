// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! GF(64) arithmetic for the P25 Reed-Solomon codes: the field of x^6 + x + 1, tables built at compile time.

/// (exp doubled to 128 entries, log) tables of GF(64) with primitive polynomial x^6 + x + 1 (0x43).
const fn tables() -> ([u8; 128], [u8; 64]) {
    let mut exp = [0u8; 128];
    let mut log = [0u8; 64];
    let mut x: u32 = 1;
    let mut i = 0;
    while i < 63 {
        exp[i] = x as u8;
        log[x as usize] = i as u8;
        x <<= 1;
        if x & 64 != 0 {
            x ^= 0x43;
        }
        i += 1;
    }
    let mut i = 63;
    while i < 128 {
        exp[i] = exp[i - 63];
        i += 1;
    }
    (exp, log)
}
static T: ([u8; 128], [u8; 64]) = tables();

/// Product of two field elements (0..63; callers mask).
pub fn mul(a: u8, b: u8) -> u8 {
    if a != 0 && b != 0 {
        T.0[T.1[a as usize & 63] as usize + T.1[b as usize & 63] as usize]
    } else {
        0
    }
}

/// Quotient a / b (0 when a is 0; b must not be 0).
pub fn div(a: u8, b: u8) -> u8 {
    if a != 0 {
        T.0[(T.1[a as usize & 63] as usize + 63 - T.1[b as usize & 63] as usize) % 63]
    } else {
        0
    }
}

/// alpha to the power `i` (any sign, taken modulo 63).
pub fn pw(i: i32) -> u8 {
    T.0[i.rem_euclid(63) as usize]
}
