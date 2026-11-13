// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! DMR block product codes and interleavers (ETSI TS 102 361-1 annex B): BPTC (196,96) of control blocks and rate 1/2 data (B.1.1), the variable length
//! BPTC of the embedded link control (B.2.1), the short LC in the CACH (B.2.3), the CACH burst with its TACT (B.4.1), the rate 3/4 trellis code
//! (B.2.4) and rate 1 data (B.2.5).
use crate::bits::{bits_to_bytes, get_bits, Out};
use crate::codes::{checksum5, crc8_short, hamming743_decode, hamming743_parity, hamming_correct, hamming_parity, Hamming};

/// Sequential number of the encoder matrix element (row r, column c) of table B.2; R(3) has index 0.
fn bptc_index(r: usize, c: usize) -> usize {
    1 + r * 15 + c
}

/// Positions (row * 15 + column) of I(95)..I(0) in the nine information rows: row 0 has three reserved bits in front.
fn bptc_info_positions() -> [usize; 96] {
    let mut out = [0usize; 96];
    let mut k = 0;
    for r in 0..9usize {
        for c in (if r == 0 { 3 } else { 0 })..11usize {
            out[k] = r * 15 + c;
            k += 1;
        }
    }
    out
}

/// BPTC (196,96) encoder: `info96[0]` is I(95); returns the 196 bits in transmit order.
pub fn bptc196_encode(info96: &[u8; 96]) -> [u8; 196] {
    let mut m = [[0u8; 15]; 13];
    let pos = bptc_info_positions();
    for i in 0..96 {
        m[pos[i] / 15][pos[i] % 15] = info96[i] & 1;
    }
    for row in m.iter_mut().take(9) {
        let mut w = 0u32;
        for &x in row.iter().take(11) {
            w = (w << 1) | u32::from(x);
        }
        let p = hamming_parity(Hamming::H15113, w);
        for j in 0..4 {
            row[11 + j] = ((p >> (3 - j)) & 1) as u8;
        }
    }
    for c in 0..15 {
        let mut w = 0u32;
        for row in m.iter().take(9) {
            w = (w << 1) | u32::from(row[c]);
        }
        let p = hamming_parity(Hamming::H1393, w);
        for j in 0..4 {
            m[9 + j][c] = ((p >> (3 - j)) & 1) as u8;
        }
    }
    let mut tx = [0u8; 196];
    for r in 0..13 {
        for c in 0..15 {
            tx[(bptc_index(r, c) * 181) % 196] = m[r][c];
        }
    }
    tx
}

/// BPTC (196,96) decoder with up to six row / column correction passes: the bits corrected, or -1 when the checks do not clear.
pub fn bptc196_decode(tx: &[u8; 196], info96: &mut [u8; 96]) -> i32 {
    let mut m = [[0u8; 15]; 13];
    for r in 0..13 {
        for c in 0..15 {
            m[r][c] = tx[(bptc_index(r, c) * 181) % 196] & 1;
        }
    }
    let mut corrected = 0;
    let mut clean = false;
    let mut iter = 0;
    while iter < 6 && !clean {
        clean = true;
        for c in 0..15 {
            let mut w = 0u32;
            for row in m.iter() {
                w = (w << 1) | u32::from(row[c]);
            }
            let e = hamming_correct(Hamming::H1393, &mut w);
            if e > 0 {
                for (r, row) in m.iter_mut().enumerate() {
                    row[c] = ((w >> (12 - r)) & 1) as u8;
                }
                corrected += 1;
                clean = false;
            } else if e < 0 {
                clean = false;
            }
        }
        for row in m.iter_mut().take(9) {
            let mut w = 0u32;
            for &x in row.iter() {
                w = (w << 1) | u32::from(x);
            }
            let e = hamming_correct(Hamming::H15113, &mut w);
            if e > 0 {
                for (c, x) in row.iter_mut().enumerate() {
                    *x = ((w >> (14 - c)) & 1) as u8;
                }
                corrected += 1;
                clean = false;
            } else if e < 0 {
                clean = false;
            }
        }
        iter += 1;
    }
    if !clean {
        // one last check, without correcting: the loop may have ended just after a successful pass
        clean = true;
        for c in 0..15 {
            if !clean {
                break;
            }
            let mut w = 0u32;
            for row in m.iter() {
                w = (w << 1) | u32::from(row[c]);
            }
            if hamming_parity(Hamming::H1393, w >> 4) != (w & 15) {
                clean = false;
            }
        }
        for row in m.iter().take(9) {
            if !clean {
                break;
            }
            let mut w = 0u32;
            for &x in row.iter() {
                w = (w << 1) | u32::from(x);
            }
            if hamming_parity(Hamming::H15113, w >> 4) != (w & 15) {
                clean = false;
            }
        }
        if !clean {
            return -1;
        }
    }
    let pos = bptc_info_positions();
    for i in 0..96 {
        info96[i] = m[pos[i] / 15][pos[i] % 15];
    }
    corrected
}

/// Embedded LC encoder (B.2.1): 72 bit link control + 5 bit checksum as a 8x16 matrix read column-wise into four 32 bit fragments.
pub fn emb_lc_encode(lc: &[u8; 9], frag: &mut [[u8; 32]; 4]) {
    let mut m = [[0u8; 16]; 8];
    let mut lc_bits = [0u8; 72];
    Out::new(&mut lc_bits).put_bytes(lc);
    let cs = u32::from(checksum5(lc));
    let mut k = 0;
    for r in 0..7usize {
        let n_info = if r < 2 { 11 } else { 10 };
        for c in 0..n_info {
            m[r][c] = lc_bits[k];
            k += 1;
        }
        if r >= 2 {
            m[r][10] = ((cs >> (6 - r)) & 1) as u8; // CS(4) in row 2 down to CS(0) in row 6
        }
        let mut w = 0u32;
        for c in 0..11 {
            w = (w << 1) | u32::from(m[r][c]);
        }
        let p = hamming_parity(Hamming::H16114, w);
        for j in 0..5 {
            m[r][11 + j] = ((p >> (4 - j)) & 1) as u8;
        }
    }
    for c in 0..16 {
        let mut x = 0u8;
        for row in m.iter().take(7) {
            x ^= row[c];
        }
        m[7][c] = x;
    }
    for (f, fr) in frag.iter_mut().enumerate() {
        for (i, o) in fr.iter_mut().enumerate() {
            let s = f * 32 + i; // the columns are read top to bottom, left to right
            *o = m[s % 8][s / 8];
        }
    }
}

/// Embedded LC decoder: the bits corrected, -1 for an uncorrectable matrix (`lc` untouched), -2 when the checksum is wrong (`lc` written).
pub fn emb_lc_decode(frag: &[[u8; 32]; 4], lc: &mut [u8; 9]) -> i32 {
    let mut m = [[0u8; 16]; 8];
    for (f, fr) in frag.iter().enumerate() {
        for (i, &x) in fr.iter().enumerate() {
            let s = f * 32 + i;
            m[s % 8][s / 8] = x & 1;
        }
    }
    let mut corrected = 0;
    for _ in 0..3 {
        let mut changed = false;
        for row in m.iter_mut().take(7) {
            let mut w = 0u32;
            for &x in row.iter() {
                w = (w << 1) | u32::from(x);
            }
            let e = hamming_correct(Hamming::H16114, &mut w);
            if e > 0 {
                for (c, x) in row.iter_mut().enumerate() {
                    *x = ((w >> (15 - c)) & 1) as u8;
                }
                corrected += 1;
                changed = true;
            } else if e < 0 {
                return -1;
            }
        }
        // column parity: a single wrong bit in a column that rows could not see (a parity bit in the bottom row) is fixed here
        for c in 0..16 {
            let mut x = 0u8;
            for row in m.iter() {
                x ^= row[c];
            }
            if x != 0 {
                // the Hamming rows are right at this point, so the fault is in the parity row
                m[7][c] ^= 1;
                corrected += 1;
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    let mut bits = [0u8; 72];
    let mut n = 0;
    for (r, row) in m.iter().enumerate().take(7) {
        let n_info = if r < 2 { 11 } else { 10 };
        for &x in row.iter().take(n_info) {
            bits[n] = x;
            n += 1;
        }
    }
    bits_to_bytes(&bits, 0, 72, lc);
    let mut cs = 0u32;
    for row in m.iter().take(7).skip(2) {
        cs = (cs << 1) | u32::from(row[10]);
    }
    if cs != u32::from(checksum5(lc)) {
        return -2;
    }
    corrected
}

/// Short LC encoder (B.2.3): 28 bits + CRC-8 as three (17,12) rows plus a parity row, read column-wise into four 17 bit pieces.
pub fn short_lc_encode(lc28: u32, piece: &mut [[u8; 17]; 4]) {
    let all = (u64::from(lc28 & 0x0FFF_FFFF) << 8) | u64::from(crc8_short(lc28 & 0x0FFF_FFFF));
    let mut m = [[0u8; 17]; 4];
    for r in 0..3usize {
        let info = ((all >> (24 - 12 * r)) & 0xFFF) as u32;
        let p = hamming_parity(Hamming::H17123, info);
        for j in 0..12 {
            m[r][j] = ((info >> (11 - j)) & 1) as u8;
        }
        for j in 0..5 {
            m[r][12 + j] = ((p >> (4 - j)) & 1) as u8;
        }
    }
    for c in 0..17 {
        m[3][c] = m[0][c] ^ m[1][c] ^ m[2][c];
    }
    for (f, pc) in piece.iter_mut().enumerate() {
        for (i, o) in pc.iter_mut().enumerate() {
            let s = f * 17 + i;
            *o = m[s % 4][s / 4];
        }
    }
}

/// Short LC decoder: the bits corrected (`lc28` set), -1 uncorrectable, -2 CRC mismatch (`lc28` set).
pub fn short_lc_decode(piece: &[[u8; 17]; 4], lc28: &mut u32) -> i32 {
    let mut m = [[0u8; 17]; 4];
    for (f, pc) in piece.iter().enumerate() {
        for (i, &x) in pc.iter().enumerate() {
            let s = f * 17 + i;
            m[s % 4][s / 4] = x & 1;
        }
    }
    let mut corrected = 0;
    let mut all: u64 = 0;
    for row in m.iter().take(3) {
        let mut w = 0u32;
        for &x in row.iter() {
            w = (w << 1) | u32::from(x);
        }
        let e = hamming_correct(Hamming::H17123, &mut w);
        if e < 0 {
            return -1;
        }
        corrected += e;
        all = (all << 12) | u64::from(w >> 5);
    }
    *lc28 = ((all >> 8) as u32) & 0x0FFF_FFFF;
    if (all & 0xFF) as u8 != crc8_short(*lc28) {
        return -2;
    }
    corrected
}

/// Transmit order of the 24 CACH bits as (kind, index): kind 0 AT, 1 TC, 2 LS1, 3 LS0, 4 parity bit H(index) (H(2) first on the air), 5 payload bit P(index)
/// (P(16) first). The TACT sits at positions 0, 4, 8, 12, 14, 18 and 22 (MMDVM DMRTX.cpp and OP25 dmr_const.h agree); the order within it is table B.17.
const CACH_ORDER: [(u8, u8); 24] = [
    (0, 0),
    (5, 16),
    (5, 15),
    (5, 14),
    (1, 0),
    (5, 13),
    (5, 12),
    (5, 11),
    (2, 0),
    (5, 10),
    (5, 9),
    (5, 8),
    (3, 0),
    (5, 7),
    (4, 2),
    (5, 6),
    (5, 5),
    (5, 4),
    (4, 1),
    (5, 3),
    (5, 2),
    (5, 1),
    (4, 0),
    (5, 0),
];

/// CACH burst (B.4.1): TACT (access type, channel, LCSS, Hamming parity) interleaved with 17 payload bits.
pub fn cach_encode(at: i32, tc: i32, lcss: i32, payload17: u32) -> [u8; 24] {
    let info = (((at & 1) << 3) | ((tc & 1) << 2) | (lcss & 3)) as u32;
    let par = hamming743_parity(info);
    let mut out = [0u8; 24];
    for (o, &(kind, idx)) in out.iter_mut().zip(CACH_ORDER.iter()) {
        let v = match kind {
            0 => (info >> 3) & 1,
            1 => (info >> 2) & 1,
            2 => (info >> 1) & 1,
            3 => info & 1,
            4 => (par >> idx) & 1,
            _ => (payload17 >> idx) & 1,
        };
        *o = v as u8;
    }
    out
}

/// Decoded CACH: TACT fields and the 17 payload bits.
pub struct Cach {
    /// Access type.
    pub at: i32,
    /// TDMA channel.
    pub tc: i32,
    /// Link control start / stop.
    pub lcss: i32,
    /// The 17 payload bits (Short LC fragment).
    pub payload17: u32,
    /// TACT bit errors corrected.
    pub errors: i32,
}

/// CACH decoder: payload always, the TACT corrected; `None` when the Hamming (7,4) check fails (`payload17` is returned in the second field then).
pub fn cach_decode(in24: &[u8; 24]) -> (Option<Cach>, u32) {
    let mut info = 0u32;
    let mut par = 0u32;
    let mut payload17 = 0u32;
    for (&x, &(kind, idx)) in in24.iter().zip(CACH_ORDER.iter()) {
        let v = u32::from(x & 1);
        match kind {
            0 => info |= v << 3,
            1 => info |= v << 2,
            2 => info |= v << 1,
            3 => info |= v,
            4 => par |= v << idx,
            _ => payload17 |= v << idx,
        }
    }
    match hamming743_decode((info << 3) | par) {
        None => (None, payload17),
        Some((e, fixed)) => (
            Some(Cach { at: ((fixed >> 3) & 1) as i32, tc: ((fixed >> 2) & 1) as i32, lcss: (fixed & 3) as i32, payload17, errors: e }),
            payload17,
        ),
    }
}

const TRELLIS_NEXT: [[u8; 8]; 8] = [
    // table B.7: state, input tribit -> constellation point
    [0, 8, 4, 12, 2, 10, 6, 14],
    [4, 12, 2, 10, 6, 14, 0, 8],
    [1, 9, 5, 13, 3, 11, 7, 15],
    [5, 13, 3, 11, 7, 15, 1, 9],
    [3, 11, 7, 15, 1, 9, 5, 13],
    [7, 15, 1, 9, 5, 13, 3, 11],
    [2, 10, 6, 14, 0, 8, 4, 12],
    [6, 14, 0, 8, 4, 12, 2, 10],
];
const CONSTELLATION: [[i8; 2]; 16] = [
    // table B.8: point -> first and second dibit as 4FSK symbols
    [1, -1],
    [-1, -1],
    [3, -3],
    [-3, -3],
    [-3, -1],
    [3, -1],
    [-1, -3],
    [1, -3],
    [-3, 3],
    [3, 3],
    [-1, 1],
    [1, 1],
    [1, 3],
    [-1, 3],
    [3, 1],
    [-3, 1],
];

/// Table B.9: transmit position k carries encoder dibit f(k): four runs of 26, 24, 24 and 24 positions.
fn trellis_interleave(k: usize) -> usize {
    let (base, j) = if k < 26 {
        (0, k)
    } else if k < 50 {
        (2, k - 26)
    } else if k < 74 {
        (4, k - 50)
    } else {
        (6, k - 74)
    };
    base + 8 * (j / 2) + (j & 1)
}

/// 4FSK symbol (+3, +1, -1, -3) to dibit (table 10.3).
pub fn symbol_to_dibit(s: i32) -> u32 {
    match s {
        3 => 1,
        1 => 0,
        -1 => 2,
        _ => 3,
    }
}
/// Dibit to 4FSK symbol: 01 -> +3, 00 -> +1, 10 -> -1, 11 -> -3.
pub fn dibit_to_symbol(d: u32) -> i32 {
    match d {
        1 => 3,
        0 => 1,
        2 => -1,
        _ => -3,
    }
}

/// Rate 3/4 trellis encoder (B.2.4): 18 data bytes -> 98 dibits (196 bits) in transmit order.
pub fn trellis34_encode(data: &[u8; 18]) -> [u8; 196] {
    let mut bits = [0u8; 144];
    Out::new(&mut bits).put_bytes(data);
    let mut sym = [0i32; 98];
    let mut state = 0usize;
    for i in 0..49usize {
        let t = if i < 48 { get_bits(&bits, i * 3, 3) as usize } else { 0 }; // 48 tribits, then the flushing tribit 000
        let pt = TRELLIS_NEXT[state][t] as usize;
        state = t;
        sym[2 * i] = i32::from(CONSTELLATION[pt][0]);
        sym[2 * i + 1] = i32::from(CONSTELLATION[pt][1]);
    }
    let mut tx = [0u8; 196];
    let mut o = Out::new(&mut tx);
    for k in 0..98 {
        o.put(u64::from(symbol_to_dibit(sym[trellis_interleave(k)])), 2);
    }
    tx
}

/// Rate 3/4 trellis decoder: `symbols` are the 98 received 4-level amplitudes in transmit order (soft values fine). Returns the number of dibits that
/// differ from the best path's re-encoding, or -1 when no path ends in the zero state (`data` untouched then).
pub fn trellis34_decode(symbols: &[f32; 98], data: &mut [u8; 18]) -> i32 {
    let mut enc = [0f32; 98];
    for k in 0..98 {
        enc[trellis_interleave(k)] = symbols[k];
    }
    let mut metric = [0f32; 8];
    let mut from = [[0u8; 8]; 49];
    for (s, m) in metric.iter_mut().enumerate() {
        *m = if s == 0 { 0.0 } else { 1e30 };
    }
    for i in 0..49usize {
        let mut nm = [1e30f32; 8];
        for s in 0..8usize {
            if metric[s] > 1e29 {
                continue;
            }
            for t in 0..8usize {
                if i == 48 && t != 0 {
                    continue; // the last tribit is the zero flush
                }
                let pt = TRELLIS_NEXT[s][t] as usize;
                let a = enc[2 * i] - f32::from(CONSTELLATION[pt][0]);
                let b = enc[2 * i + 1] - f32::from(CONSTELLATION[pt][1]);
                let m = metric[s] + a * a + b * b;
                if m < nm[t] {
                    nm[t] = m;
                    from[i][t] = s as u8;
                }
            }
        }
        metric = nm;
    }
    if metric[0] > 1e29 {
        return -1;
    }
    let mut tri = [0u8; 49];
    let mut st = 0usize;
    for i in (0..49).rev() {
        tri[i] = st as u8;
        st = from[i][st] as usize;
    }
    let mut bits = [0u8; 144];
    let mut o = Out::new(&mut bits);
    for &t in tri.iter().take(48) {
        o.put(u64::from(t), 3);
    }
    bits_to_bytes(&bits, 0, 144, data);
    // how many dibits the winning path disagrees with, as a measure of the damage
    let re = trellis34_encode(data);
    let mut bad = 0;
    for k in 0..98 {
        let v = symbols[k];
        let hard = if v >= 2.0 {
            3
        } else if v >= 0.0 {
            1
        } else if v >= -2.0 {
            -1
        } else {
            -3
        };
        if symbol_to_dibit(hard) != (u32::from(re[2 * k]) << 1 | u32::from(re[2 * k + 1])) {
            bad += 1;
        }
    }
    bad
}

/// Rate 1 encoder (B.2.5): 192 data bits placed in 196 with four zero pad bits after the first 96.
pub fn rate1_encode(data: &[u8; 24]) -> [u8; 196] {
    let mut d = [0u8; 192];
    Out::new(&mut d).put_bytes(data);
    let mut tx = [0u8; 196];
    tx[..96].copy_from_slice(&d[..96]);
    tx[100..].copy_from_slice(&d[96..]);
    tx
}

/// Rate 1 decoder: the 24 data bytes, and whether the four pad bits are zero.
pub fn rate1_decode(tx: &[u8; 196], data: &mut [u8; 24]) -> bool {
    let mut d = [0u8; 192];
    d[..96].copy_from_slice(&tx[..96]);
    d[96..].copy_from_slice(&tx[100..]);
    bits_to_bytes(&d, 0, 192, data);
    (tx[96] | tx[97] | tx[98] | tx[99]) == 0
}
