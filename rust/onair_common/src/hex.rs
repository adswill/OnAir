// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Hex text to and from bytes, into caller-owned buffers.

const LOWER: &[u8; 16] = b"0123456789abcdef";
const UPPER: &[u8; 16] = b"0123456789ABCDEF";

/// Writes two hex digits per input byte. Returns the length written, or None when `out` is too small.
pub fn encode(data: &[u8], out: &mut [u8], upper: bool) -> Option<usize> {
    let t = if upper { UPPER } else { LOWER };
    if out.len() < data.len() * 2 {
        return None;
    }
    for (i, &b) in data.iter().enumerate() {
        out[2 * i] = t[(b >> 4) as usize];
        out[2 * i + 1] = t[(b & 15) as usize];
    }
    Some(data.len() * 2)
}

fn nib(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

/// Parses hex digits (either case, no separators). None for an odd length, a non-hex byte, or a too small `out`.
pub fn decode(s: &[u8], out: &mut [u8]) -> Option<usize> {
    if s.len() % 2 != 0 || out.len() < s.len() / 2 {
        return None;
    }
    for i in 0..s.len() / 2 {
        out[i] = (nib(s[2 * i])? << 4) | nib(s[2 * i + 1])?;
    }
    Some(s.len() / 2)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn round_trip() {
        let mut t = [0u8; 8];
        assert_eq!(encode(&[0xDE, 0xAD, 0x01, 0xFF], &mut t, false), Some(8));
        assert_eq!(&t, b"dead01ff");
        let mut b = [0u8; 4];
        assert_eq!(decode(b"DEad01Ff", &mut b), Some(4));
        assert_eq!(b, [0xDE, 0xAD, 0x01, 0xFF]);
        assert_eq!(decode(b"abc", &mut b), None);
        assert_eq!(decode(b"zz", &mut b), None);
        assert_eq!(encode(&[1], &mut t[..1], true), None);
    }
}
