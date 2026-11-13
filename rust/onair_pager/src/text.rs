// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Message text of POCSAG and FLEX: 7-bit alphanumeric characters, 4-bit numeric digits and the "does this look like real text" test that the
//! automatic read mode of the receiver uses. Bits come one per byte, first transmitted bit first.

/// POCSAG numeric character set (ITU-R M.584-2 table): index = 4-bit value.
pub const NUM_POCSAG: &[u8; 16] = b"0123456789*U -][";
/// FLEX numeric character set (public descriptions): index = 4-bit value.
pub const NUM_FLEX: &[u8; 16] = b"0123456789 U -][";

/// Reads the bits as 7-bit characters (first bit least significant); keeps printable ASCII, CR, LF and tab and drops padding (NUL, ETX, EOT).
/// Returns the length written; stops when `out` is full (it is never smaller than `bits.len() / 7` in the callers).
pub fn alpha_from_bits(bits: &[u8], out: &mut [u8]) -> usize {
    let mut n = 0;
    for ch in bits.chunks_exact(7) {
        let mut c = 0u32;
        for (k, b) in ch.iter().enumerate() {
            c |= u32::from(b & 1) << k;
        }
        let keep = (0x20..0x7F).contains(&c) || c == 10 || c == 13 || c == 9;
        if keep && n < out.len() {
            out[n] = c as u8;
            n += 1;
        }
    }
    n
}

/// Reads the bits as 4-bit digits through `table` (12 is dropped when `drop_fill`), then trims trailing spaces. Returns the length written.
pub fn numeric_from_bits(bits: &[u8], table: &[u8; 16], drop_fill: bool, out: &mut [u8]) -> usize {
    let mut n = 0;
    for ch in bits.chunks_exact(4) {
        let mut d = 0usize;
        for (k, b) in ch.iter().enumerate() {
            d |= usize::from(b & 1) << k;
        }
        if drop_fill && d == 12 {
            continue;
        }
        if n < out.len() {
            out[n] = table[d];
            n += 1;
        }
    }
    while n > 0 && out[n - 1] == b' ' {
        n -= 1;
    }
    n
}

fn is_alpha(c: u32) -> bool {
    (u32::from(b'A')..=u32::from(b'Z')).contains(&c) || (u32::from(b'a')..=u32::from(b'z')).contains(&c)
}
fn is_lower(c: u32) -> bool {
    (u32::from(b'a')..=u32::from(b'z')).contains(&c)
}
fn is_upper(c: u32) -> bool {
    (u32::from(b'A')..=u32::from(b'Z')).contains(&c)
}

/// The message bits read as 7-bit characters look like real text: mostly letters, digits and spaces, few odd signs, no run of control
/// characters, at least 3 characters (the C++ reference's heuristic, same thresholds).
pub fn looks_alpha(bits: &[u8]) -> bool {
    let nch = bits.len() / 7;
    let ch = |i: usize| -> u32 {
        let mut c = 0u32;
        for k in 0..7 {
            c |= u32::from(bits[i * 7 + k] & 1) << k;
        }
        c
    };
    let mut n = nch;
    while n > 0 {
        let c = ch(n - 1);
        if c == 0 || c == 3 || c == 4 {
            n -= 1;
        } else {
            break;
        }
    }
    if n < 3 {
        return false;
    }
    let (mut letters, mut digits, mut spaces, mut odd, mut ctrl, mut run, mut max_run, mut pairs, mut low_up, mut prev) =
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0u32);
    for i in 0..n {
        let c = ch(i);
        if is_alpha(prev) && is_alpha(c) {
            pairs += 1;
            if is_lower(prev) && is_upper(c) {
                low_up += 1; // "xY" is rare in text, common in noise
            }
        }
        prev = c;
        let is_ctrl = (c < 0x20 && c != 10 && c != 13) || c == 0x7F;
        if is_ctrl {
            ctrl += 1;
            run += 1;
            if run > max_run {
                max_run = run;
            }
        } else {
            run = 0;
        }
        if is_alpha(c) {
            letters += 1;
        } else if (u32::from(b'0')..=u32::from(b'9')).contains(&c) {
            digits += 1;
        } else if c == 32 || c == 10 || c == 13 {
            spaces += 1;
        } else if !is_ctrl && !b".,:;-/()#&'!?@+=*\"".contains(&(c as u8)) {
            odd += 1; // [\]^_`{|}~ and the like
        }
    }
    let n = n as i32;
    if n < 12 {
        // short: chance alone would pass the ratios below, so be strict
        if n < 6 || ctrl != 0 || odd != 0 || letters + digits + spaces < n - 1 {
            return false;
        }
        return letters * 2 >= n && low_up * 8 <= pairs;
    }
    if max_run >= 2 || ctrl * 10 > n {
        return false;
    }
    if (letters + digits + spaces) * 20 < n * 17 {
        return false; // 85 % at least
    }
    if odd * 10 > n || low_up * 8 > pairs {
        return false;
    }
    letters * 4 >= n // text has letters: a quarter at least
}

#[cfg(test)]
mod tests {
    use super::*;
    fn bits7(s: &[u8]) -> ([u8; 700], usize) {
        let mut b = [0u8; 700];
        for (i, c) in s.iter().enumerate() {
            for k in 0..7 {
                b[i * 7 + k] = (c >> k) & 1;
            }
        }
        (b, s.len() * 7)
    }
    #[test]
    fn alpha_and_looks() {
        let (b, n) = bits7(b"Hello World 12\x04\x04");
        let mut o = [0u8; 32];
        let l = alpha_from_bits(&b[..n], &mut o);
        assert_eq!(&o[..l], b"Hello World 12");
        assert!(looks_alpha(&b[..n]));
        let (b, n) = bits7(b"xYqZ~\x01\x02\x7f");
        assert!(!looks_alpha(&b[..n]));
    }
    #[test]
    fn numeric() {
        // digits 1,2,fill(12),3 then trailing spaces trimmed
        let nib = |d: u8| [d & 1, (d >> 1) & 1, (d >> 2) & 1, (d >> 3) & 1];
        let mut bits = [0u8; 20];
        for (i, d) in [1u8, 2, 12, 3, 12].iter().enumerate() {
            bits[i * 4..i * 4 + 4].copy_from_slice(&nib(*d));
        }
        let mut o = [0u8; 8];
        let l = numeric_from_bits(&bits, NUM_POCSAG, false, &mut o);
        assert_eq!(&o[..l], b"12 3");
        let l = numeric_from_bits(&bits, NUM_FLEX, true, &mut o);
        assert_eq!(&o[..l], b"123");
    }
}
