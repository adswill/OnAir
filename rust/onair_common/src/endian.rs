// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Endian helpers on byte slices. Reads past the end give 0 in the missing bytes; writes return false instead of panicking.

pub fn read_u16_be(b: &[u8], pos: usize) -> u16 {
    read_be(b, pos, 2) as u16
}
pub fn read_u16_le(b: &[u8], pos: usize) -> u16 {
    read_le(b, pos, 2) as u16
}
pub fn read_u32_be(b: &[u8], pos: usize) -> u32 {
    read_be(b, pos, 4) as u32
}
pub fn read_u32_le(b: &[u8], pos: usize) -> u32 {
    read_le(b, pos, 4) as u32
}
pub fn read_u64_be(b: &[u8], pos: usize) -> u64 {
    read_be(b, pos, 8)
}
pub fn read_u64_le(b: &[u8], pos: usize) -> u64 {
    read_le(b, pos, 8)
}

/// `n` (0..=8) bytes, most significant first.
pub fn read_be(b: &[u8], pos: usize, n: usize) -> u64 {
    let mut v = 0u64;
    for i in 0..n.min(8) {
        v = (v << 8) | pos.checked_add(i).and_then(|p| b.get(p)).copied().unwrap_or(0) as u64;
    }
    v
}
/// `n` (0..=8) bytes, least significant first.
pub fn read_le(b: &[u8], pos: usize, n: usize) -> u64 {
    let mut v = 0u64;
    for i in 0..n.min(8) {
        v |= (pos.checked_add(i).and_then(|p| b.get(p)).copied().unwrap_or(0) as u64) << (8 * i);
    }
    v
}

/// Writes the low `n` (0..=8) bytes of `v`, most significant first. False (nothing written) when it does not fit.
pub fn write_be(b: &mut [u8], pos: usize, n: usize, v: u64) -> bool {
    if n > 8 || pos.checked_add(n).map_or(true, |e| e > b.len()) {
        return false;
    }
    for i in 0..n {
        b[pos + i] = (v >> (8 * (n - 1 - i))) as u8;
    }
    true
}
/// Writes the low `n` (0..=8) bytes of `v`, least significant first.
pub fn write_le(b: &mut [u8], pos: usize, n: usize, v: u64) -> bool {
    if n > 8 || pos.checked_add(n).map_or(true, |e| e > b.len()) {
        return false;
    }
    for i in 0..n {
        b[pos + i] = (v >> (8 * i)) as u8;
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn round_trip() {
        let mut b = [0u8; 8];
        assert!(write_be(&mut b, 0, 4, 0x1234_5678));
        assert_eq!(read_u32_be(&b, 0), 0x1234_5678);
        assert_eq!(read_u32_le(&b, 0), 0x7856_3412);
        assert_eq!(read_u16_be(&b, 1), 0x3456);
        assert!(write_le(&mut b, 2, 6, 0x0605_0403_0201));
        assert_eq!(read_u64_le(&b, 0) >> 16, 0x0605_0403_0201);
        assert!(!write_le(&mut b, 7, 2, 0));
        assert_eq!(read_u16_be(&b, 7), (b[7] as u16) << 8, "missing byte reads 0");
        assert_eq!(read_u64_be(&b, usize::MAX), 0);
    }
}
