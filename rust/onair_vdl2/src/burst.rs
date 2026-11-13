// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! VDL Mode 2 burst body and HDLC layer: gathering the data and FEC octets of a burst into Reed-Solomon blocks and correcting them, the HDLC deframer (0x7E flags,
//! zero-bit insertion, abort on seven ones) and the FCS (CRC-16/X.25, residue 0xF0B8). ICAO Annex 10 Vol III Part I section 6.3.2 (interleaving), ISO 3309.
use crate::bits::{Layout, RS_K, RS_N};
use crate::rs;

/// Most Reed-Solomon blocks a burst can have: the 17 bit header length is at most 131071 bits = 16384 octets = 66 blocks.
pub const MAX_BLOCKS: usize = 70;

fn get_octet(bits: &[u8], at: usize) -> u8 {
    let mut v = 0u8;
    for b in 0..8 {
        v |= (bits.get(at + b).copied().unwrap_or(0) & 1) << b;
    }
    v
}

/// Gathers the descrambled bits after the header (octets sent column by column across the blocks, data then FEC; bits past the end of `bits` read as 0),
/// corrects every block and writes the first `len_bits` data bits (one per byte) into `out`. Returns the number of bits written and the octets the code
/// corrected so far (also when a block cannot be corrected: then the first value is `None`; so for a burst of more than `MAX_BLOCKS` blocks).
pub fn burst_data(bits: &[u8], l: &Layout, len_bits: u32, out: &mut [u8]) -> (Option<usize>, i32) {
    if l.blocks < 0 || l.blocks as usize > MAX_BLOCKS {
        return (None, 0);
    }
    let blocks = l.blocks as usize;
    let (last_len, last_fec) = (l.last_len.max(0) as usize, l.last_fec.max(0) as usize);
    let mut rows = [[0u8; RS_N]; MAX_BLOCKS];
    let mut k = 0usize;
    for c in 0..RS_K {
        for (r, row) in rows.iter_mut().enumerate().take(blocks) {
            if r == blocks - 1 && c >= last_len {
                continue;
            }
            row[c] = get_octet(bits, 8 * k);
            k += 1;
        }
    }
    let fec_rows = if last_fec != 0 { blocks } else { blocks.saturating_sub(1) };
    for c in 0..RS_N - RS_K {
        for (r, row) in rows.iter_mut().enumerate().take(fec_rows) {
            if r == blocks - 1 && c >= last_fec {
                continue;
            }
            row[RS_K + c] = get_octet(bits, 8 * k);
            k += 1;
        }
    }
    let mut fixed = 0;
    for (r, row) in rows.iter_mut().enumerate().take(blocks) {
        let np = if r == blocks - 1 { last_fec } else { 6 };
        if np == 0 {
            continue;
        }
        let f = rs::decode(row, np as i32);
        if f < 0 {
            return (None, fixed);
        }
        fixed += f;
    }
    let mut n = 0usize;
    for (r, row) in rows.iter().enumerate().take(blocks) {
        let cnt = if r == blocks - 1 { last_len } else { RS_K };
        for &octet in row.iter().take(cnt) {
            for b in 0..8 {
                if n < len_bits as usize && n < out.len() {
                    out[n] = (octet >> b) & 1;
                    n += 1;
                }
            }
        }
    }
    (Some(n), fixed)
}

fn crc_reg(p: &[u8]) -> u16 {
    let mut c: u16 = 0xFFFF;
    for &b in p {
        c ^= b as u16;
        for _ in 0..8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0x8408 } else { c >> 1 };
        }
    }
    c
}

/// The FCS to send for the frame octets `p` (low octet first): the complement of the CRC register.
pub fn fcs(p: &[u8]) -> u16 {
    !crc_reg(p)
}

/// True when the frame, including its two FCS octets, leaves the CRC register at the residue 0xF0B8.
pub fn fcs_ok(p: &[u8]) -> bool {
    p.len() >= 2 && crc_reg(p) == 0xF0B8
}

/// Extracts the frames between flags from the bit stream (one bit per byte): octets are written one after the other into `out`, their lengths into `lens`.
/// Frames need at least two octets and a whole number of octets, and must not have seen seven ones in a row. Returns `(frames, octets)`, or `None` when `out` or
/// `lens` is too small (about `bits.len() / 8` octets and `bits.len() / 16` frames are enough).
pub fn hdlc_frames(bits: &[u8], out: &mut [u8], lens: &mut [u32]) -> Option<(usize, usize)> {
    let (mut nframes, mut base) = (0usize, 0usize);
    let (mut cur, mut ones) = (0usize, 0u32);
    let (mut sync, mut abort, mut over) = (false, false, false);
    for &bit in bits {
        if bit != 0 {
            ones += 1;
            if ones > 6 {
                abort = true;
                cur = 0;
            } else {
                over |= push(out, base, &mut cur, 1, abort);
            }
            continue;
        }
        if ones == 5 {
            ones = 0; // inserted zero
            continue;
        }
        if ones == 6 {
            // flag: drop its leading zero and six ones, then finish the frame
            cur = cur.saturating_sub(7);
            if sync && !abort && cur >= 16 && cur % 8 == 0 {
                let len = cur / 8;
                if over || nframes >= lens.len() {
                    return None;
                }
                lens[nframes] = len as u32;
                nframes += 1;
                base += len;
            }
            cur = 0;
            abort = false;
            over = false;
            sync = true;
            ones = 0;
            continue;
        }
        over |= push(out, base, &mut cur, 0, abort);
        ones = 0;
    }
    Some((nframes, base))
}

/// Appends one bit (least significant bit first within an octet) to the frame being collected at `out[base..]`; bits of an aborted frame are only counted.
/// True when the bit did not fit into `out`.
fn push(out: &mut [u8], base: usize, cur: &mut usize, bit: u8, abort: bool) -> bool {
    let mut over = false;
    if !abort {
        let at = base + *cur / 8;
        if at >= out.len() {
            over = true;
        } else {
            if *cur % 8 == 0 {
                out[at] = 0;
            }
            out[at] |= bit << (*cur % 8);
        }
    }
    *cur += 1;
    over
}
