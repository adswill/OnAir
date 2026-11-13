// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! HDLC frame layer of AIS: CRC-16 X.25, bit stuffing, NRZI, octet order and frame search. Values other than 0 and 1 in a bit stream are
//! treated exactly as the C++ code treats them (nonzero tests, raw comparisons), so both implementations agree on any input.

use onair_common::crc::{reflect, Crc, CRC16_X25};
use onair_common::nrzi;

pub const FCS_GOOD: u16 = 0xF0B8;
const FLAG: [u8; 8] = [0, 1, 1, 1, 1, 1, 1, 0];

/// CRC-16/X-25 register after feeding `bits` (one per byte, transmission order, LSB of each octet first), in the reflected form
/// the HDLC residue 0xF0B8 is quoted in. Nonzero tests only, so odd values behave as in the C++ code.
pub fn fcs_register(bits: &[u8]) -> u16 {
    let mut c = Crc::new(CRC16_X25);
    for &b in bits {
        c.update_bit(b & 1 != 0);
    }
    reflect(c.register(), 16) as u16
}

pub fn fcs_ok(frame: &[u8]) -> bool {
    frame.len() >= 16 && fcs_register(frame) == FCS_GOOD
}

/// A 0 after every five 1s. Returns the output length, or None when `out` is too small (n + n/5 + 1 always fits).
pub fn stuff(inp: &[u8], out: &mut [u8]) -> Option<usize> {
    let mut n = 0;
    let mut ones = 0;
    for &b in inp {
        *out.get_mut(n)? = b;
        n += 1;
        if b != 0 {
            ones += 1;
            if ones == 5 {
                *out.get_mut(n)? = 0;
                n += 1;
                ones = 0;
            }
        } else {
            ones = 0;
        }
    }
    Some(n)
}

/// Removes the stuffed 0s. Err(()) when six 1s in a row appear (flag or abort); Ok(len) otherwise. `out` must hold `inp.len()`.
#[allow(clippy::result_unit_err)]
pub fn destuff(inp: &[u8], out: &mut [u8]) -> Result<usize, ()> {
    let mut n = 0;
    let mut ones = 0;
    for &b in inp {
        if ones == 5 {
            if b != 0 {
                return Err(());
            }
            ones = 0;
            continue;
        }
        *out.get_mut(n).ok_or(())? = b;
        n += 1;
        ones = if b != 0 { ones + 1 } else { 0 };
    }
    Ok(n)
}

pub fn nrzi_encode(data: &[u8], start: i32, out: &mut [u8]) -> Option<usize> {
    nrzi::encode_hdlc(data, start, out)
}

pub fn nrzi_decode(line: &[u8], prev: i32, out: &mut [u8]) -> Option<usize> {
    nrzi::decode_hdlc(line, prev, out)
}

/// HDLC sends every octet least significant bit first: reverse each group of 8 (the last group on its own).
pub fn reverse_octets(b: &mut [u8]) {
    for c in b.chunks_mut(8) {
        c.reverse();
    }
}

const MAX_STRETCH: usize = 1100;

/// Frames between flags in an NRZI-decoded stream. Good frames (payload without FCS, in message table order) are appended to `out`, their
/// lengths to `lens`. Returns (good, bad). Frames that do not fit the buffers are dropped (`out.len() >= bits.len()` and
/// `lens.len() >= bits.len() / 64 + 2` always fit).
pub fn hdlc_frames(d: &[u8], out: &mut [u8], lens: &mut [usize]) -> (usize, i32) {
    let mut good = 0usize;
    let mut bad = 0i32;
    let mut used = 0usize;
    let mut prev: Option<usize> = None;
    let mut i = 0usize;
    let mut scratch = [0u8; MAX_STRETCH];
    while i + 8 <= d.len() {
        if d[i..i + 8] == FLAG {
            if let Some(f) = prev {
                let a = f + 8;
                let e = i;
                if !(e < a + 56 || e - a > MAX_STRETCH) {
                    let raw = &d[a..e];
                    let mut alt = 0;
                    let mut zeros = 0;
                    for k in 2..=12usize {
                        if k > f {
                            break;
                        }
                        if d[f - k] != d[f - k + 1] {
                            alt += 1;
                        }
                    }
                    for k in 1..=12usize {
                        if k > f {
                            break;
                        }
                        if d[f - k] == 0 {
                            zeros += 1;
                        }
                    }
                    let ok = match destuff(raw, &mut scratch) {
                        Ok(n) if fcs_ok(&scratch[..n]) => Some(n - 16),
                        _ => None,
                    };
                    match ok {
                        Some(n) => {
                            let fr = &mut scratch[..n];
                            reverse_octets(fr);
                            if good < lens.len() && used + n <= out.len() {
                                out[used..used + n].copy_from_slice(fr);
                                lens[good] = n;
                                used += n;
                                good += 1;
                            }
                        }
                        None => {
                            if alt >= 9 || zeros >= 11 {
                                bad += 1;
                            }
                        }
                    }
                }
            }
            prev = Some(i);
            i += 8;
        } else {
            i += 1;
        }
    }
    (good, bad)
}
