// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Parity and check-sequence repair: a block is repaired when the flipped bits that the parity errors point at explain the
//! check-sequence residue.
use crate::crc::{self, MAX_SYN};

/// The result of [`repair_block`].
pub struct Repair {
    /// The block is good (the text is repaired in place).
    pub ok: bool,
    /// Bits flipped.
    pub fixed_bits: i32,
    /// Bytes with wrong parity that were seen.
    pub parity_errors: i32,
}

/// True when the residue is that of one flipped bit in the check bytes themselves.
fn in_check_bytes(c: u16) -> bool {
    (0..2).any(|e| (0..8).any(|b| crc::syn(e, b) == c))
}

/// Each byte with wrong parity has one bit wrong. The choice of a bit for each of the bytes is stepped through like the digits of a counter (the
/// first byte is the most significant digit); the first choice whose combined syndrome equals the residue wins.
fn with_parity(txt: &mut [u8], c: u16, pr: &[usize], flips: &mut i32) -> bool {
    let mut bit = [0usize; 8];
    let n = pr.len();
    loop {
        let mut r = c;
        for i in 0..n {
            r ^= crc::syn(txt.len() - pr[i] + 1, bit[i]);
        }
        // what is left may be an error in the check bytes themselves
        if r == 0 || in_check_bytes(r) {
            for i in 0..n {
                txt[pr[i]] ^= 1 << bit[i];
            }
            *flips += n as i32;
            return true;
        }
        let mut i = n;
        loop {
            if i == 0 {
                return false;
            }
            i -= 1;
            bit[i] += 1;
            if bit[i] < 8 {
                break;
            }
            bit[i] = 0;
        }
    }
}

/// With no parity error: an error of two bits in one byte is tried.
fn double(txt: &mut [u8], c: u16, flips: &mut i32) -> bool {
    if in_check_bytes(c) {
        return true;
    }
    let len = txt.len();
    for (k, byte) in txt.iter_mut().enumerate() {
        let e = len - k + 1;
        for i in 0..8 {
            for j in i + 1..8 {
                if c ^ crc::syn(e, i) ^ crc::syn(e, j) == 0 {
                    *byte ^= (1 << i) | (1 << j);
                    *flips += 2;
                    return true;
                }
            }
        }
    }
    false
}

/// `txt`: the bytes after SOH up to and including the suffix (with their parity bits, as received); `check`: the two check bytes. Up to
/// `max_parity_errors` bytes with wrong parity are allowed: each gets one bit flipped so that the check comes out (or leaves an error that only
/// the check bytes carry). With no parity error, an error of two bits in one byte is tried.
pub fn repair_block(txt: &mut [u8], check: [u8; 2], max_parity_errors: i32) -> Repair {
    let mut r = Repair { ok: false, fixed_bits: 0, parity_errors: 0 };
    let len = txt.len();
    if len < 13 || len + 2 >= MAX_SYN {
        return r;
    }
    let mut pr = [0usize; 8];
    let mut pn = 0usize;
    for (i, &b) in txt.iter().enumerate() {
        if !crc::parity_ok(b) {
            if pn < 8 {
                pr[pn] = i;
            }
            pn += 1;
        }
    }
    r.parity_errors = pn as i32;
    // more than 8 wrong bytes cannot be repaired (the C++ original keeps room for 8 only)
    if pn as i64 > max_parity_errors as i64 || pn > 8 {
        return r;
    }
    let mut c = crc::crc(txt, 0);
    c = crc::update(c, check[0]);
    c = crc::update(c, check[1]);
    if pn > 0 {
        if !with_parity(txt, c, &pr[..pn], &mut r.fixed_bits) {
            return r;
        }
    } else if c != 0 && !double(txt, c, &mut r.fixed_bits) {
        return r;
    }
    r.ok = txt.iter().all(|&b| crc::parity_ok(b));
    r
}
