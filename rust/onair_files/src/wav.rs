// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! RIFF / WAVE headers of two channel IQ recordings (SDR programs write them): the `fmt ` chunk, the choice of sample format, where the `data` chunk ends and the
//! centre frequency that is often part of the file name. Walking the chunks (seeking in the file) stays in C++.

/// The fields of a `fmt ` chunk that matter.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct WavFmt {
    /// Format tag: 1 PCM, 3 float; for WAVE_FORMAT_EXTENSIBLE (0xFFFE) the sub-format tag.
    pub tag: u32,
    /// Channels.
    pub channels: u32,
    /// Sample rate (Hz).
    pub rate: u32,
    /// Bits per sample.
    pub bits: u32,
}

fn rd16(b: &[u8], i: usize) -> u32 {
    (b.get(i).copied().unwrap_or(0) as u32) | ((b.get(i + 1).copied().unwrap_or(0) as u32) << 8)
}

fn rd32(b: &[u8], i: usize) -> u32 {
    rd16(b, i) | (rd16(b, i + 2) << 16)
}

/// Reads the body of a `fmt ` chunk (`len` is the chunk's declared length, `body` the bytes that were read, zero padded to at least 16 by the caller like the
/// reference did; missing bytes read as 0).
pub fn parse_fmt(body: &[u8], len: u32) -> WavFmt {
    let mut tag = rd16(body, 0);
    if tag == 0xFFFE && len >= 26 {
        tag = rd16(body, 24);
    }
    WavFmt { tag, channels: rd16(body, 2), rate: rd32(body, 4), bits: rd16(body, 14) }
}

/// Sample format of a WAV that can play as IQ: 0 = error "needs two channels", 1.. formats: returns (code, format) where code 0 = ok, 1 = not two channels,
/// 2 = sample format not supported. Formats use the `FileFormat` numbering: 1 CU8 (8-bit PCM), 2 CF32 (32-bit float), 3 CI16 (16-bit PCM).
pub fn classify(f: &WavFmt) -> (u32, i32) {
    if f.channels != 2 {
        return (1, 0);
    }
    if f.tag == 1 && f.bits == 16 {
        (0, 3)
    } else if f.tag == 1 && f.bits == 8 {
        (0, 1)
    } else if f.tag == 3 && f.bits == 32 {
        (0, 2)
    } else {
        (2, 0)
    }
}

/// End of the sample data as an absolute file offset: 0 (until the end of the file) when the chunk says length 0 or 0xFFFFFFFF (streaming writers) or claims
/// more than the file holds.
pub fn data_end(offset: u64, len: u32, file_size: u64) -> u64 {
    // plain unsigned arithmetic, like the reference
    let end = offset.wrapping_add(len as u64);
    if len == 0 || len == 0xFFFF_FFFF || end > file_size {
        0
    } else {
        end
    }
}

/// The centre frequency in Hz that a file name holds as "..._100.000MHz_..." (case ignored), 0 if it does not.
pub fn center_from_name(name: &[u8]) -> f64 {
    let low = |c: u8| c.to_ascii_lowercase();
    let mut m = None;
    if name.len() >= 3 {
        for i in 0..=name.len() - 3 {
            if low(name[i]) == b'm' && low(name[i + 1]) == b'h' && low(name[i + 2]) == b'z' {
                m = Some(i);
                break;
            }
        }
    }
    let m = match m {
        Some(m) => m,
        None => return 0.0,
    };
    let mut b = m;
    while b > 0 && (name[b - 1].is_ascii_digit() || name[b - 1] == b'.') {
        b -= 1;
    }
    if b >= m {
        return 0.0;
    }
    atof_prefix(&name[b..m]) * 1e6
}

/// `atof` of a text made of digits and dots: the longest prefix `digits [. digits*]` or `. digits`, 0 when there is none.
pub fn atof_prefix(t: &[u8]) -> f64 {
    let mut j = 0;
    while j < t.len() && t[j].is_ascii_digit() {
        j += 1;
    }
    let int_digits = j;
    let mut frac = 0;
    if j < t.len() && t[j] == b'.' {
        let mut k = j + 1;
        while k < t.len() && t[k].is_ascii_digit() {
            k += 1;
        }
        frac = k - j - 1;
        if int_digits > 0 || frac > 0 {
            j = k;
        }
    }
    if int_digits == 0 && frac == 0 {
        return 0.0;
    }
    crate::json::parse_f64(&t[..j])
}
