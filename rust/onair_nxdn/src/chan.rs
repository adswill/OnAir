// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The NXDN coded channels (NXDN TS 1-A section 4.4): SACCH, FACCH1 and CAC. Information bits plus CRC plus four tail zeros go through the K=5
//! convolutional code (G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4), are punctured, and block interleaved; decoding is a soft Viterbi
//! (positive soft value = a 1) followed by the CRC check. Fixed buffers, no allocation.
use crate::crc;

/// One of the coded channels.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Ch {
    /// Slow associated control channel: 26 bits + CRC-6.
    Sacch = 0,
    /// Fast associated control channel 1: 80 bits + CRC-12.
    Facch1 = 1,
    /// Common access channel: 155 bits + CRC-16.
    Cac = 2,
}

impl Ch {
    /// Channel from the C enum value (0 SACCH, 1 FACCH1, 2 CAC).
    pub fn from_i32(v: i32) -> Option<Ch> {
        match v {
            0 => Some(Ch::Sacch),
            1 => Some(Ch::Facch1),
            2 => Some(Ch::Cac),
            _ => None,
        }
    }
}

struct Spec {
    info: usize,
    crc: usize,
    rows: usize,
    cols: usize,
    punct: &'static [u8],
}
const PUN_SACCH: [u8; 12] = [1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0];
const PUN_FACCH: [u8; 4] = [1, 0, 1, 1];
const PUN_CAC: [u8; 14] = [1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1];

/// Size and interleaver of a channel: information bits, CRC bits, interleaver rows and columns, puncturing pattern.
fn spec(c: Ch) -> Spec {
    match c {
        Ch::Sacch => Spec { info: 26, crc: 6, rows: 12, cols: 5, punct: &PUN_SACCH },
        Ch::Facch1 => Spec { info: 80, crc: 12, rows: 16, cols: 9, punct: &PUN_FACCH },
        Ch::Cac => Spec { info: 155, crc: 16, rows: 12, cols: 25, punct: &PUN_CAC },
    }
}

/// Longest message incl. CRC and tail (CAC: 155 + 16 + 4).
const MAXN: usize = 175;
/// Longest air block (CAC).
const MAXL: usize = 300;

/// Information bits of a channel before the CRC (26, 80, 155).
pub fn info_bits(c: Ch) -> usize {
    spec(c).info
}
/// Bits a channel occupies on air (60, 144, 300).
pub fn air_bits(c: Ch) -> usize {
    let p = spec(c);
    p.rows * p.cols
}

/// The CRC of the channel over `b`.
fn crc_of(c: Ch, b: &[u8]) -> u32 {
    match c {
        Ch::Sacch => crc::crc6(b),
        Ch::Facch1 => crc::crc12(b),
        Ch::Cac => crc::crc16(b),
    }
}

/// K=5 convolutional encoder: n bits in, 2n bits out.
fn conv_encode(inp: &[u8], out: &mut [u8]) {
    let mut s: u32 = 0;
    for (i, &x) in inp.iter().enumerate() {
        let b = (x & 1) as u32;
        out[2 * i] = (b ^ ((s >> 2) & 1) ^ ((s >> 3) & 1)) as u8;
        out[2 * i + 1] = (b ^ (s & 1) ^ ((s >> 1) & 1) ^ ((s >> 3) & 1)) as u8;
        s = ((s << 1) | b) & 15;
    }
}

/// Soft Viterbi over 2n soft values (positive = 1) for the K=5 code; the path ends in state 0. Writes n bits.
fn viterbi(soft: &[f32], out: &mut [u8]) {
    let n = out.len();
    let mut pm = [0f32; 16];
    let mut nm = [0f32; 16];
    for (s, p) in pm.iter_mut().enumerate() {
        *p = if s == 0 { 0.0 } else { -1e9 };
    }
    let mut dec = [0u16; MAXN];
    for i in 0..n {
        let a = soft[2 * i];
        let b = soft[2 * i + 1];
        let mut d: u16 = 0;
        for ns in 0..16usize {
            let bit = ns & 1;
            let mut best = -2e9f32;
            let mut bp = 0usize;
            for hi in 0..2usize {
                let s = (ns >> 1) | (hi << 3);
                let g1 = bit ^ ((s >> 2) & 1) ^ ((s >> 3) & 1);
                let g2 = bit ^ (s & 1) ^ ((s >> 1) & 1) ^ ((s >> 3) & 1);
                let m = pm[s] + (if g1 != 0 { a } else { -a }) + (if g2 != 0 { b } else { -b });
                if m > best {
                    best = m;
                    bp = hi;
                }
            }
            nm[ns] = best;
            if bp != 0 {
                d |= 1 << ns;
            }
        }
        dec[i] = d;
        pm = nm;
    }
    let mut s = 0usize;
    for i in (0..n).rev() {
        out[i] = (s & 1) as u8;
        let hi = ((dec[i] >> s) & 1) as usize;
        s = (s >> 1) | (hi << 3);
    }
}

/// Interleaver: air position k carries punctured bit `perm(k)`.
fn perm(p: &Spec, k: usize) -> usize {
    k / p.cols + p.rows * (k % p.cols)
}

/// Encodes `info` (info_bits bits, one per byte) into `air` (air_bits bits): CRC, tail, convolutional code, puncturing, interleaving.
/// `None` when a buffer is too short.
pub fn encode(c: Ch, info: &[u8], air: &mut [u8]) -> Option<()> {
    let p = spec(c);
    let l = p.rows * p.cols;
    if info.len() < p.info || air.len() < l {
        return None;
    }
    let n = p.info + p.crc + 4;
    let mut u = [0u8; MAXN];
    let mut cod = [0u8; 2 * MAXN];
    let mut pun = [0u8; MAXL];
    for i in 0..p.info {
        u[i] = info[i] & 1;
    }
    let r = crc_of(c, &u[..p.info]);
    for i in 0..p.crc {
        u[p.info + i] = ((r >> (p.crc - 1 - i)) & 1) as u8;
    }
    conv_encode(&u[..n], &mut cod[..2 * n]);
    let mut j = 0;
    for i in 0..2 * n {
        if p.punct[i % p.punct.len()] != 0 {
            pun[j] = cod[i];
            j += 1;
        }
    }
    for (k, a) in air.iter_mut().enumerate().take(l) {
        *a = pun[perm(&p, k)];
    }
    Some(())
}

/// Result of [`decode`]: whether the CRC held and, then, the channel bits that differ from the re-encoded result.
pub struct Decoded {
    /// CRC check passed.
    pub ok: bool,
    /// Channel bit errors (0 unless `ok`).
    pub errs: i32,
}

/// Decodes `soft` (air_bits soft values, positive = 1) into `info` (info_bits bits). `None` when a buffer is too short.
pub fn decode(c: Ch, soft: &[f32], info: &mut [u8]) -> Option<Decoded> {
    let p = spec(c);
    let l = p.rows * p.cols;
    if soft.len() < l || info.len() < p.info {
        return None;
    }
    let n = p.info + p.crc + 4;
    let mut pun = [0f32; MAXL];
    let mut cod = [0f32; 2 * MAXN];
    for (k, &s) in soft.iter().enumerate().take(l) {
        pun[perm(&p, k)] = s;
    }
    let mut j = 0;
    for i in 0..2 * n {
        if p.punct[i % p.punct.len()] != 0 {
            cod[i] = pun[j];
            j += 1;
        }
    }
    let mut u = [0u8; MAXN];
    viterbi(&cod[..2 * n], &mut u[..n]);
    let mut r: u32 = 0;
    for i in 0..p.crc {
        r = (r << 1) | u[p.info + i] as u32;
    }
    info[..p.info].copy_from_slice(&u[..p.info]);
    let ok = crc_of(c, &u[..p.info]) == r;
    let mut errs = 0;
    if ok {
        let mut air = [0u8; MAXL];
        encode(c, &u[..p.info], &mut air[..l])?;
        for k in 0..l {
            if (soft[k] > 0.0) != (air[k] != 0) {
                errs += 1;
            }
        }
    }
    Some(Decoded { ok, errs })
}
