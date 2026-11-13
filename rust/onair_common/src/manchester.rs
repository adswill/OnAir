// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Manchester coding on unpacked bits. Each data bit becomes two half-bit levels. IEEE 802.3: 0 = 10, 1 = 01
//! (a 1 is a low-to-high transition). G.E. Thomas is the inverse: 1 = 10, 0 = 01.

/// Encodes `data` into `2 * data.len()` half bits. None when `out` is too small.
pub fn encode(data: &[u8], ieee: bool, out: &mut [u8]) -> Option<usize> {
    if out.len() < data.len() * 2 {
        return None;
    }
    for (i, &d) in data.iter().enumerate() {
        let one = (d & 1 != 0) == ieee; // true: low then high (01)
        out[2 * i] = !one as u8;
        out[2 * i + 1] = one as u8;
    }
    Some(data.len() * 2)
}

/// Decodes pairs of half bits. None on an invalid pair (00 or 11, no mid-bit transition), an odd length or a too small `out`.
pub fn decode(half: &[u8], ieee: bool, out: &mut [u8]) -> Option<usize> {
    if half.len() % 2 != 0 || out.len() < half.len() / 2 {
        return None;
    }
    for i in 0..half.len() / 2 {
        let (a, b) = (half[2 * i] & 1, half[2 * i + 1] & 1);
        if a == b {
            return None;
        }
        out[i] = ((b == 1) == ieee) as u8;
    }
    Some(half.len() / 2)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ieee_and_thomas() {
        let mut h = [0u8; 4];
        assert_eq!(encode(&[1, 0], true, &mut h), Some(4));
        assert_eq!(h, [0, 1, 1, 0]);
        assert_eq!(encode(&[1, 0], false, &mut h), Some(4));
        assert_eq!(h, [1, 0, 0, 1]);
        let mut d = [0u8; 2];
        assert_eq!(decode(&[0, 1, 1, 0], true, &mut d), Some(2));
        assert_eq!(d, [1, 0]);
        assert_eq!(decode(&[0, 0, 1, 0], true, &mut d), None);
        assert_eq!(decode(&[0, 1, 1], true, &mut d), None);
    }
}
