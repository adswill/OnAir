// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Gray code (adjacent values differ in one bit), as used by QAM/PSK/CSS symbol mappings.

pub fn to_gray(v: u32) -> u32 {
    v ^ (v >> 1)
}
pub fn from_gray(g: u32) -> u32 {
    let mut v = g;
    let mut s = 1;
    while s < 32 {
        v ^= v >> s;
        s <<= 1;
    }
    v
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn gray() {
        assert_eq!([to_gray(0), to_gray(1), to_gray(2), to_gray(3), to_gray(4)], [0, 1, 3, 2, 6]);
        for v in (0..5000).chain([u32::MAX, 0x8000_0000]) {
            assert_eq!(from_gray(to_gray(v)), v);
        }
        for v in 0..1000u32 {
            assert_eq!((to_gray(v) ^ to_gray(v + 1)).count_ones(), 1);
        }
    }
}
