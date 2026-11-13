// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! DRM data service helpers (ES 201 980 clause 6.4.3.6 and 6.6): MJD calendar conversions (clause 6.4.3.9), the payload of an MSC data group, and the text
//! of a received object for display (control characters out, ISO 8859-1 turned into UTF-8 when the bytes are not UTF-8) with the heuristic that decides
//! whether an object is text.
use onair_dab::crc::{crc16, tail16};

/// The calendar date of a Modified Julian Date: (year, month, day).
pub fn mjd_to_date(mjd: i32) -> (i32, i32, i32) {
    let j = mjd.wrapping_add(2400001 + 32044);
    let g = j / 146097;
    let dg = j % 146097;
    let c = (dg / 36524 + 1) * 3 / 4;
    let dc = dg - c * 36524;
    let b = dc / 1461;
    let db = dc % 1461;
    let a = (db / 365 + 1) * 3 / 4;
    let da = db - a * 365;
    let yy = g * 400 + c * 100 + b * 4 + a;
    let mm = (da * 5 + 308) / 153 - 2;
    let dd = da - (mm + 4) * 153 / 5 + 122;
    (yy - 4800 + (mm + 2) / 12, (mm + 2) % 12 + 1, dd + 1)
}

/// The Modified Julian Date of a calendar date.
pub fn date_to_mjd(y: i32, m: i32, d: i32) -> i32 {
    let a = (14 - m) / 12;
    let yy = y + 4800 - a;
    let mm = m + 12 * a - 3;
    let jdn = d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
    jdn - 2400001
}

/// The payload of an MSC data group (EN 300 401 clause 5.3.3.1: after the headers, before the CRC): (offset, length), `None` when the group is damaged.
pub fn group_payload(d: &[u8]) -> Option<(usize, usize)> {
    let mut n = d.len();
    if n < 3 {
        return None;
    }
    let (ext, crc, seg, ua) = (d[0] & 0x80 != 0, d[0] & 0x40 != 0, d[0] & 0x20 != 0, d[0] & 0x10 != 0);
    if crc {
        if n < 5 || crc16(&d[..n - 2]) != tail16(d) {
            return None;
        }
        n -= 2;
    }
    let mut p = 2 + if ext { 2 } else { 0 } + if seg { 2 } else { 0 };
    if ua {
        if p + 1 > n {
            return None;
        }
        p += 1 + (d[p] & 0x0F) as usize;
    }
    if p > n {
        return None;
    }
    Some((p, n - p))
}

/// Largest text a data service object shows.
pub const TEXT_BYTES: usize = 16384;

/// Is `s` valid UTF-8 (the loose check of the reference: lead byte classes and continuation bits, no overlong or range checks)?
fn valid_utf8(s: &[u8]) -> bool {
    let mut i = 0;
    while i < s.len() {
        let c = s[i];
        let n = if c < 0x80 {
            1
        } else if (c >> 5) == 6 {
            2
        } else if (c >> 4) == 14 {
            3
        } else if (c >> 3) == 30 {
            4
        } else {
            0
        };
        if n == 0 || i + n > s.len() {
            return false;
        }
        for j in 1..n {
            if (s[i + j] >> 6) != 2 {
                return false;
            }
        }
        i += n;
    }
    true
}

/// The text of a received object for display: the first 16 kbyte, carriage returns dropped, other control characters (except line feed and tab) as spaces,
/// bytes >= 0x80 as ISO 8859-1 when the bytes are not valid UTF-8. `out` needs twice the input length (at most 32768 bytes); returns the length.
pub fn display_text(b: &[u8], out: &mut [u8]) -> usize {
    let raw = &b[..b.len().min(TEXT_BYTES)];
    let u8ok = valid_utf8(raw);
    let mut w = 0;
    let mut put = |c: u8| {
        if w < out.len() {
            out[w] = c;
            w += 1;
        }
    };
    for &c in raw {
        if c == b'\r' {
            continue;
        }
        if c < 0x20 && c != b'\n' && c != b'\t' {
            put(b' ');
            continue;
        }
        if c >= 0x80 && !u8ok {
            put(0xC0 | (c >> 6));
            put(0x80 | (c & 0x3F));
            continue;
        }
        put(c);
    }
    w
}

/// Does an object look like text: no NUL in its first 512 bytes (at least 4 of them) and fewer than 2 % control characters other than line breaks and tabs?
pub fn looks_text(b: &[u8]) -> bool {
    let n = b.len().min(512);
    if n < 4 {
        return false;
    }
    let mut odd = 0usize;
    for &c in &b[..n] {
        if c == 0 {
            return false;
        }
        if c < 0x20 && c != b'\n' && c != b'\r' && c != b'\t' {
            odd += 1;
        }
    }
    odd * 50 < n
}
