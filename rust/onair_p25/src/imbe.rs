// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! P25 IMBE voice frame interleave (TIA-102.BABA, as in DSD): one frame is 72 dibits as sent; the eight code vectors c0..c7 (23, 23, 23, 23, 15, 15, 15, 7
//! bits) are laid out the way mbelib takes them (`char fr[8][23]`, 184 bytes row major). The vocoder itself stays in C++.

const W: [u8; 72] = [
    0, 2, 4, 1, 3, 5, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 5, 1, 3, 6, 0, 2, 5,
    1, 3, 6, 0, 2, 5, 1, 3, 7, 0, 2, 5, 1, 3, 7, 0, 2, 5, 1, 4, 7, 0, 3, 5, 2, 4, 7,
];
const X: [u8; 72] = [
    22, 20, 10, 20, 18, 0, 20, 18, 8, 18, 16, 13, 18, 16, 6, 16, 14, 11, 16, 14, 4, 14, 12, 9, 14, 12, 2, 12, 10, 7, 12, 10, 0, 10, 8, 5,
    10, 8, 13, 8, 6, 3, 8, 6, 11, 6, 4, 1, 6, 4, 9, 4, 2, 6, 4, 2, 7, 2, 0, 4, 2, 0, 5, 0, 13, 2, 0, 21, 3, 21, 11, 0,
];
const Y: [u8; 72] = [
    1, 3, 5, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 5, 1, 3, 6, 0, 2, 5, 1, 3, 6,
    0, 2, 5, 1, 3, 6, 0, 2, 5, 1, 3, 7, 0, 2, 5, 1, 4, 7, 0, 3, 5, 2, 4, 7, 1, 3, 5,
];
const Z: [u8; 72] = [
    21, 19, 1, 21, 19, 9, 19, 17, 14, 19, 17, 7, 17, 15, 12, 17, 15, 5, 15, 13, 10, 15, 13, 3, 13, 11, 8, 13, 11, 1, 11, 9, 6, 11, 9, 14,
    9, 7, 4, 9, 7, 12, 7, 5, 2, 7, 5, 10, 5, 3, 0, 5, 3, 8, 3, 1, 5, 3, 1, 6, 1, 14, 3, 1, 22, 4, 22, 12, 1, 22, 20, 2,
];
/// Length in bits of each code vector.
const LEN: [usize; 8] = [23, 23, 23, 23, 15, 15, 15, 7];

/// Spreads 72 dibits (values 0..3) into the 8 x 23 code vector matrix (cleared first).
pub fn deinterleave(d: &[u8; 72], fr: &mut [u8; 184]) {
    fr.iter_mut().for_each(|x| *x = 0);
    for i in 0..72 {
        fr[W[i] as usize * 23 + X[i] as usize] = (d[i] >> 1) & 1;
        fr[Y[i] as usize * 23 + Z[i] as usize] = d[i] & 1;
    }
}

/// Gathers the 8 x 23 matrix back into 72 dibits.
pub fn interleave(fr: &[u8; 184], d: &mut [u8; 72]) {
    for i in 0..72 {
        d[i] = ((fr[W[i] as usize * 23 + X[i] as usize] & 1) << 1) | (fr[Y[i] as usize * 23 + Z[i] as usize] & 1);
    }
}

/// The 144 bits c0 first (each row's used bits).
pub fn flatten(fr: &[u8; 184], bits: &mut [u8; 144]) {
    let mut p = 0;
    for r in 0..8 {
        for j in 0..LEN[r] {
            bits[p] = fr[r * 23 + j] & 1;
            p += 1;
        }
    }
}

/// The inverse of [`flatten`] (matrix cleared first).
pub fn unflatten(bits: &[u8; 144], fr: &mut [u8; 184]) {
    fr.iter_mut().for_each(|x| *x = 0);
    let mut p = 0;
    for r in 0..8 {
        for j in 0..LEN[r] {
            fr[r * 23 + j] = bits[p] & 1;
            p += 1;
        }
    }
}
