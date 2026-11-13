// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! GPS text carried in the D-STAR slow data: NMEA sentences ($GPRMC / $GPGGA, also GN, GL ...; checksum checked) and D-PRS lines
//! (`$$CRCxxxx,CALL>DEST,...:!DDMM.mmN/DDDMM.mmE...`, CRC-CCITT of the text after the comma). Numbers are read with the C library (`atof`, `atoi`,
//! `strtoul`, `lround`) like the C++ reference, so every value is identical bit for bit. The result codes tell whether the reference would have left the
//! output untouched (the early checks) or filled it before failing.
use crate::ffi::cfn;
use crate::header::crc;

/// A position fix.
#[derive(Clone, Copy, PartialEq, Debug, Default)]
pub struct Fix {
    /// Latitude in degrees, north positive.
    pub lat: f64,
    /// Longitude in degrees, east positive.
    pub lon: f64,
    /// Altitude in metres.
    pub alt_m: f64,
    /// Speed in km/h.
    pub speed_kmh: f64,
    /// Course in degrees.
    pub course: i32,
    /// Altitude present.
    pub alt_valid: bool,
    /// Speed and course present.
    pub vel_valid: bool,
    /// Came from a D-PRS line.
    pub aprs: bool,
}

/// What a parser did to the caller's `Fix`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Outcome {
    /// Rejected by an early check: the reference leaves the fix untouched.
    Untouched,
    /// Rejected after the fix was reset or partly filled: the fix holds what the reference had written.
    Partial,
    /// Accepted.
    Parsed,
}

/// The two hex digits "%02X" of the XOR of `body`.
pub fn nmea_checksum(body: &[u8]) -> [u8; 2] {
    let x = body.iter().fold(0u8, |a, &c| a ^ c);
    const H: &[u8; 16] = b"0123456789ABCDEF";
    [H[(x >> 4) as usize], H[(x & 15) as usize]]
}

/// CRC of a D-PRS line (the text after "$$CRCxxxx,").
pub fn dprs_crc(s: &[u8]) -> u16 {
    crc(s)
}

/// The `idx`-th comma separated field of `s`, or `None` when there are fewer.
fn field(s: &[u8], idx: usize) -> Option<&[u8]> {
    s.split(|&c| c == b',').nth(idx)
}

/// Number of comma separated fields (at least one).
fn fields(s: &[u8]) -> usize {
    s.split(|&c| c == b',').count()
}

/// `ddmm.mmmm` (or `dddmm.mmmm`) and a hemisphere letter into degrees. Like the reference it writes `out` before it rejects a bad hemisphere.
fn coord(v: &[u8], h: &[u8], deg_digits: usize, out: &mut f64) -> bool {
    if v.len() < deg_digits + 2 || h.is_empty() {
        return false;
    }
    if !v.iter().all(|&c| c == b'.' || c.is_ascii_digit()) {
        return false;
    }
    let deg = cfn::atof(&v[..deg_digits]);
    let min = cfn::atof(&v[deg_digits..]);
    if min >= 60.0 {
        return false;
    }
    *out = deg + min / 60.0;
    if h[0] == b'S' || h[0] == b'W' {
        *out = -*out;
    } else if h[0] != b'N' && h[0] != b'E' {
        return false;
    }
    true
}

/// Parses `$GPRMC` / `$GPGGA` (and the other talker ids): position, speed and course, or altitude.
pub fn parse_nmea(line: &[u8], f: &mut Fix) -> Outcome {
    if line.len() < 10 || line[0] != b'$' {
        return Outcome::Untouched;
    }
    let Some(star) = line.iter().position(|&c| c == b'*') else { return Outcome::Untouched };
    if star + 3 > line.len() {
        return Outcome::Untouched;
    }
    let body = &line[1..star];
    if nmea_checksum(body) != line[star + 1..star + 3] {
        return Outcome::Untouched;
    }
    let p0 = field(body, 0).unwrap_or(&[]);
    if p0.len() != 5 {
        return Outcome::Untouched;
    }
    let t = &p0[2..];
    let n = fields(body);
    let p = |i: usize| field(body, i).unwrap_or(&[]);
    *f = Fix::default();
    if t == b"RMC" && n >= 9 {
        if p(2) != b"A" {
            return Outcome::Partial;
        }
        if !coord(p(3), p(4), 2, &mut f.lat) || !coord(p(5), p(6), 3, &mut f.lon) {
            return Outcome::Partial;
        }
        if !p(7).is_empty() {
            f.speed_kmh = cfn::atof(p(7)) * 1.852;
            f.course = cfn::lround(cfn::atof(p(8))) as i32;
            f.vel_valid = true;
        }
        return Outcome::Parsed;
    }
    if t == b"GGA" && n >= 10 {
        if p(6).is_empty() || p(6) == b"0" {
            return Outcome::Partial;
        }
        if !coord(p(2), p(3), 2, &mut f.lat) || !coord(p(4), p(5), 3, &mut f.lon) {
            return Outcome::Partial;
        }
        if !p(9).is_empty() {
            f.alt_m = cfn::atof(p(9));
            f.alt_valid = true;
        }
        return Outcome::Parsed;
    }
    Outcome::Partial
}

/// Result texts of [`parse_dprs`]: the call and the comment as lengths written into the caller's buffers.
pub struct DprsText {
    /// Length of the APRS source call written to `call`.
    pub call_len: usize,
    /// Length of the comment written to `comment` (0 unless parsed).
    pub comment_len: usize,
}

/// Parses a D-PRS line (CRC checked; the CRC may include a closing carriage return that the line lost). `call` and `comment` each need `line.len()` bytes.
pub fn parse_dprs(line: &[u8], f: &mut Fix, call: &mut [u8], comment: &mut [u8]) -> (Outcome, DprsText) {
    let mut txt = DprsText { call_len: 0, comment_len: 0 };
    if line.len() < 11 || &line[..5] != b"$$CRC" || line[9] != b',' {
        return (Outcome::Untouched, txt);
    }
    let rest = &line[10..];
    let want = cfn::strtoul16(&line[5..9]) as u32;
    // the CRC covers the closing carriage return, which the line may have lost
    if u32::from(crc(rest)) != want && u32::from(crc_with_cr(rest)) != want {
        return (Outcome::Untouched, txt);
    }
    let gt = rest.iter().position(|&c| c == b'>');
    let colon = rest.iter().position(|&c| c == b':');
    let (Some(gt), Some(colon)) = (gt, colon) else { return (Outcome::Untouched, txt) };
    if colon + 20 > rest.len() {
        return (Outcome::Untouched, txt);
    }
    *f = Fix { aprs: true, ..Fix::default() };
    let n = gt.min(call.len());
    call[..n].copy_from_slice(&rest[..n]);
    txt.call_len = n;
    let pos0 = &rest[colon + 1..];
    if !matches!(pos0[0], b'!' | b'=' | b'/' | b'@') {
        return (Outcome::Partial, txt);
    }
    let pos = &pos0[if pos0[0] == b'/' || pos0[0] == b'@' { 8 } else { 1 }..]; // a time stamp first with / and @
    if pos.len() < 19 {
        return (Outcome::Partial, txt);
    }
    // DDMM.mmN / DDDMM.mmE, the symbol table between, the symbol after
    if !coord(&pos[..7], &pos[7..8], 2, &mut f.lat) || !coord(&pos[9..17], &pos[17..18], 3, &mut f.lon) {
        return (Outcome::Partial, txt);
    }
    let mut c: &[u8] = if pos.len() > 19 { &pos[19..] } else { &[] };
    if c.len() >= 7 && c[3] == b'/' && c[0].is_ascii_digit() && c[4].is_ascii_digit() {
        // course / speed in knots
        f.course = cfn::atoi(&c[..3]);
        f.speed_kmh = f64::from(cfn::atoi(&c[4..7])) * 1.852;
        f.vel_valid = true;
        c = &c[7..];
    }
    // "/A=" and six digits (feet) anywhere in the comment are taken out of it
    let alt = c.windows(3).position(|w| w == b"/A=");
    let (head, tail) = match alt {
        Some(a) if a + 9 <= c.len() => {
            f.alt_m = cfn::atof(&c[a + 3..a + 9]) * 0.3048;
            f.alt_valid = true;
            (&c[..a], &c[a + 9..])
        }
        _ => (c, &c[c.len()..]),
    };
    let mut k = 0;
    for &x in head.iter().chain(tail.iter()) {
        if k < comment.len() {
            comment[k] = x;
        }
        k += 1;
    }
    let mut k = k.min(comment.len());
    while k > 0 && matches!(comment[k - 1], b'\r' | b'\n' | b' ') {
        k -= 1;
    }
    txt.comment_len = k;
    (Outcome::Parsed, txt)
}

/// CRC of `s` followed by a carriage return.
fn crc_with_cr(s: &[u8]) -> u16 {
    let mut c: u16 = 0xFFFF;
    for &x in s.iter().chain(core::iter::once(&b'\r')) {
        c ^= u16::from(x);
        for _ in 0..8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0x8408 } else { c >> 1 };
        }
    }
    !c
}
