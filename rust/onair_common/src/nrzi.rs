// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! NRZI on unpacked bit streams (one bit per byte). `*_hdlc`: a 0 toggles the line level, a 1 keeps it (NRZI-S, HDLC/AX.25/AIS/USB).
//! `*_mark`: a 1 toggles the level. Decoding compares raw values exactly as written (no masking), so odd inputs behave the same
//! as the C++ code did.

/// HDLC NRZI encoding starting from line level `start`. None when `out` is shorter than `data`.
pub fn encode_hdlc(data: &[u8], start: i32, out: &mut [u8]) -> Option<usize> {
    let mut lv = start & 1;
    for (i, &d) in data.iter().enumerate() {
        if d == 0 {
            lv ^= 1;
        }
        *out.get_mut(i)? = lv as u8;
    }
    Some(data.len())
}

/// HDLC NRZI decoding given the previous line level `prev`: same level as before is a 1.
pub fn decode_hdlc(line: &[u8], prev: i32, out: &mut [u8]) -> Option<usize> {
    let mut p = prev & 1;
    for (i, &l) in line.iter().enumerate() {
        *out.get_mut(i)? = if l as i32 == p { 1 } else { 0 };
        p = l as i32;
    }
    Some(line.len())
}

/// NRZI-M (mark) encoding: a 1 toggles the level.
pub fn encode_mark(data: &[u8], start: i32, out: &mut [u8]) -> Option<usize> {
    let mut lv = start & 1;
    for (i, &d) in data.iter().enumerate() {
        if d != 0 {
            lv ^= 1;
        }
        *out.get_mut(i)? = lv as u8;
    }
    Some(data.len())
}

/// NRZI-M decoding: a level change is a 1.
pub fn decode_mark(line: &[u8], prev: i32, out: &mut [u8]) -> Option<usize> {
    let mut p = prev & 1;
    for (i, &l) in line.iter().enumerate() {
        *out.get_mut(i)? = if l as i32 != p { 1 } else { 0 };
        p = l as i32;
    }
    Some(line.len())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn round_trips() {
        let data = [1, 0, 0, 1, 1, 1, 0, 1, 0];
        let mut line = [0u8; 9];
        let mut back = [0u8; 9];
        assert_eq!(encode_hdlc(&data, 1, &mut line), Some(9));
        assert_eq!(line, [1, 0, 1, 1, 1, 1, 0, 0, 1]);
        assert_eq!(decode_hdlc(&line, 1, &mut back), Some(9));
        assert_eq!(back, data);
        assert_eq!(encode_mark(&data, 0, &mut line), Some(9));
        assert_eq!(decode_mark(&line, 0, &mut back), Some(9));
        assert_eq!(back, data);
        assert_eq!(encode_hdlc(&data, 0, &mut line[..3]), None);
    }
}
