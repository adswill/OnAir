// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Parity helpers.

/// Number of 1 bits is odd.
pub fn odd_parity(v: u64) -> bool {
    v.count_ones() % 2 == 1
}
/// Parity bit that makes the total number of ones even (the usual "even parity" bit).
pub fn even_parity_bit(v: u64) -> u8 {
    odd_parity(v) as u8
}
/// Parity bit that makes the total number of ones odd.
pub fn odd_parity_bit(v: u64) -> u8 {
    !odd_parity(v) as u8
}
/// XOR of all bytes (longitudinal redundancy check).
pub fn xor_bytes(data: &[u8]) -> u8 {
    data.iter().fold(0, |a, &b| a ^ b)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn parity() {
        assert!(!odd_parity(0));
        assert!(odd_parity(0b111));
        assert_eq!(even_parity_bit(0b1011), 1);
        assert_eq!(odd_parity_bit(0b1011), 0);
        assert_eq!(xor_bytes(&[1, 2, 3]), 0);
    }
}
