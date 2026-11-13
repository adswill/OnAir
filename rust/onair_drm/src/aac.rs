// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The DRM audio super frame (ES 201 980 clause 5.4): the number of audio frames of a super frame, the frame border header, and the split of a logical frame
//! into audio frames and their CRC bytes (higher protected part first, lower protected part after it).

/// Most audio frames a super frame can have in the parser's fixed arrays (the standard has 5 or 10).
pub const MAX_FRAMES: usize = 16;

/// Number of audio frames per super frame (5 or 10), 0 when the sampling rate is not allowed for the mode.
pub fn num_frames(mode_e: bool, rate_hz: i32) -> i32 {
    if !mode_e {
        match rate_hz {
            12000 => 5,
            24000 => 10,
            _ => 0,
        }
    } else {
        match rate_hz {
            24000 => 5,
            48000 => 10,
            _ => 0,
        }
    }
}

/// Bytes of the frame border header: 12 bits per border, 4 padding bits for 10 frames.
pub fn header_bytes(n: i32) -> i32 {
    ((n - 1) * 12 + if n == 10 { 4 } else { 0 } + 7) / 8
}

/// Result of `super_frame_parse`.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct AacInfo {
    /// Frames expected.
    pub num_frames: i32,
    /// Higher protected bytes of each frame.
    pub hp_bytes: i32,
    /// The borders increase and stay inside the payload.
    pub header_ok: u8,
    /// Length of each frame in `frames`.
    pub frame_len: [u32; MAX_FRAMES],
    /// The 8 bit CRC of each frame.
    pub crc: [u8; MAX_FRAMES],
}

/// Split a logical frame (`lf`: part A then part B, `len_a` bytes in part A) into `n` audio frames; their bytes are written one after the other to `frames`
/// (`lf.len()` bytes are enough). False when the structure is not consistent (`info.header_ok` says whether the borders were).
pub fn super_frame_parse(lf: &[u8], len_a: i32, n: i32, info: &mut AacInfo, frames: &mut [u8]) -> bool {
    *info = AacInfo { num_frames: n, ..Default::default() };
    let len = lf.len() as i64;
    let hdr = header_bytes(n) as i64;
    if n < 1 || n as usize > MAX_FRAMES || len < hdr + n as i64 {
        return false;
    }
    let n64 = n as i64;
    let mut hp = 0i64;
    if len_a > 0 {
        if (len_a as i64) < hdr + n64 || (len_a as i64 - hdr) % n64 != 0 {
            return false;
        }
        hp = (len_a as i64 - hdr) / n64 - 1;
    }
    info.hp_bytes = hp as i32;
    if hdr + n64 * (hp + 1) > len {
        return false;
    }
    let mut border = [0i64; MAX_FRAMES];
    let mut prev = 0i64;
    for i in 0..(n - 1) as usize {
        let bit = i * 12;
        let v = ((lf[bit / 8] as i64) << 8) | lf[bit / 8 + 1] as i64;
        let mut v = if bit % 8 == 0 { (v >> 4) & 0xFFF } else { v & 0xFFF };
        while v < prev {
            v += 4096;
        }
        border[i] = v;
        prev = v;
    }
    let payload = len - hdr - n64;
    border[n as usize - 1] = payload;
    let mut ok = true;
    let mut start = 0i64;
    for &b in border.iter().take(n as usize) {
        if b < start || b > payload || b - start < hp {
            ok = false;
        }
        start = b;
    }
    info.header_ok = ok as u8;
    if !ok {
        return false;
    }
    let hp_part = hdr as usize;
    let mut lp_pos = (hdr + n64 * (hp + 1)) as usize;
    start = 0;
    let mut w = 0usize;
    for i in 0..n as usize {
        let fl = border[i] - start;
        start = border[i];
        let hpo = hp_part + i * (hp as usize + 1);
        let lpn = (fl - hp) as usize;
        if lp_pos + lpn > lf.len() {
            info.header_ok = 0;
            return false;
        }
        let fr = [&lf[hpo..hpo + hp as usize], &lf[lp_pos..lp_pos + lpn]];
        info.crc[i] = lf[hpo + hp as usize];
        let mut k = 0u32;
        for part in fr {
            for &c in part {
                if w < frames.len() {
                    frames[w] = c;
                }
                w += 1;
                k += 1;
            }
        }
        info.frame_len[i] = k;
        lp_pos += lpn;
    }
    true
}
