// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Two-line element sets (NORAD TLE, as Celestrak publishes them): column layout, line checksum, the implied-decimal exponent fields, the epoch as a Julian date,
//! and the scan of a whole text (a file, or what a user pasted) for name / line 1 / line 2 groups. The propagator (SGP4) stays in C++.
use crate::ffi::cfn;
use crate::json::parse_f64;
use crate::Span;

/// Result of parsing one element set (all angles in degrees, as in the file).
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq)]
pub struct Tle {
    /// 0 = parsed, 1 = not a TLE (nothing else valid), 2 = fields valid but the orbit is unusable (mean motion not positive or eccentricity 1 or more).
    pub status: i32,
    /// Catalogue number.
    pub norad: i32,
    /// 1 when the name must be the catalogue number (no name given).
    pub name_is_norad: i32,
    /// Range of the name inside the name text the caller gave (or inside the scanned text), empty when `name_is_norad`.
    pub name: Span,
    /// Epoch as a UTC Julian date.
    pub epoch_jd: f64,
    /// B* drag term.
    pub bstar: f64,
    /// Inclination.
    pub incl_deg: f64,
    /// Right ascension of the ascending node.
    pub raan_deg: f64,
    /// Eccentricity (the file gives it without the decimal point).
    pub ecc: f64,
    /// Argument of perigee.
    pub argp_deg: f64,
    /// Mean anomaly.
    pub mean_anom_deg: f64,
    /// Revolutions per day.
    pub mean_motion: f64,
}

fn is_trim(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\r' | b'\n')
}

/// Range of `s` without leading and trailing blanks (space, tab, CR, LF).
pub fn trim(s: &[u8]) -> (usize, usize) {
    let mut a = 0;
    let mut b = s.len();
    while a < b && is_trim(s[a]) {
        a += 1;
    }
    while b > a && is_trim(s[b - 1]) {
        b -= 1;
    }
    (a, b)
}

/// Length of the decimal number token at the start of `t` (`[+-]? (digits [. digits*] | . digits) ([eE] [+-]? digits)?`), 0 when there is none.
pub fn decimal_len(t: &[u8]) -> usize {
    let mut j = 0;
    if matches!(t.first(), Some(b'+') | Some(b'-')) {
        j += 1;
    }
    let d0 = j;
    while matches!(t.get(j), Some(b'0'..=b'9')) {
        j += 1;
    }
    let int_digits = j - d0;
    let mut frac = 0;
    if t.get(j) == Some(&b'.') {
        let mut k = j + 1;
        while matches!(t.get(k), Some(b'0'..=b'9')) {
            k += 1;
        }
        frac = k - j - 1;
        if int_digits > 0 || frac > 0 {
            j = k;
        }
    }
    if int_digits == 0 && frac == 0 {
        return 0;
    }
    if matches!(t.get(j), Some(b'e') | Some(b'E')) {
        let mut k = j + 1;
        if matches!(t.get(k), Some(b'+') | Some(b'-')) {
            k += 1;
        }
        let e0 = k;
        while matches!(t.get(k), Some(b'0'..=b'9')) {
            k += 1;
        }
        if k > e0 {
            j = k;
        }
    }
    j
}

/// A number field: columns `pos .. pos+len` (fails if the line is shorter), blanks around it ignored, empty = 0, else the whole rest must be one decimal number.
fn num(s: &[u8], pos: usize, len: usize) -> Option<f64> {
    if s.len() < pos + len {
        return None;
    }
    let f = &s[pos..pos + len];
    let (a, b) = trim(f);
    let f = &f[a..b];
    if f.is_empty() {
        return Some(0.0);
    }
    if decimal_len(f) != f.len() {
        return None;
    }
    Some(parse_f64(f))
}

/// `strtol` over the whole text in base 10 (sign and digits, nothing else), None when it is not entirely a number; saturates like `strtol`.
fn whole_long(t: &[u8]) -> Option<i64> {
    let mut i = 0;
    let mut neg = false;
    if matches!(t.first(), Some(b'+') | Some(b'-')) {
        neg = t[0] == b'-';
        i = 1;
    }
    if i >= t.len() {
        return None;
    }
    let mut v: i64 = 0;
    for &c in &t[i..] {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v.saturating_mul(10).saturating_add((c - b'0') as i64);
    }
    Some(if neg { -v } else { v })
}

/// "-12345-6": a mantissa with an implied leading decimal point and a power of ten (the B* field of line 1).
fn exp_field(s: &[u8], pos: usize, len: usize) -> Option<f64> {
    if s.len() < pos + len {
        return None;
    }
    let f = &s[pos..pos + len];
    let (a, b) = trim(f);
    let mut f = &f[a..b];
    if f.is_empty() {
        return Some(0.0);
    }
    let mut sign = 1.0;
    if f[0] == b'-' || f[0] == b'+' {
        if f[0] == b'-' {
            sign = -1.0;
        }
        f = &f[1..];
    }
    // the first '+' or '-' after position 0 starts the exponent
    let ep = f.iter().enumerate().skip(1).find(|(_, &c)| c == b'+' || c == b'-').map(|(i, _)| i);
    let (mant, ex): (&[u8], &[u8]) = match ep {
        Some(p) => (&f[..p], &f[p..]),
        None => (f, b"0"),
    };
    let (ma, mb) = trim(mant);
    let mant = &mant[ma..mb];
    // "0." + mantissa must be one decimal number
    let mut buf = [0u8; 40];
    if mant.len() + 2 > buf.len() {
        return None;
    }
    buf[0] = b'0';
    buf[1] = b'.';
    buf[2..2 + mant.len()].copy_from_slice(mant);
    let t = &buf[..2 + mant.len()];
    if decimal_len(t) != t.len() {
        return None;
    }
    let m = parse_f64(t);
    let x = whole_long(ex)?;
    Some(sign * m * cfn::pow(10.0, x as f64))
}

fn checksum_ok(l: &[u8]) -> bool {
    if l.len() < 69 {
        return l.len() >= 68;
    }
    let mut sum = 0u32;
    for &c in &l[..68] {
        if c.is_ascii_digit() {
            sum += (c - b'0') as u32;
        } else if c == b'-' {
            sum += 1;
        }
    }
    let d = l[68];
    !d.is_ascii_digit() || sum % 10 == (d - b'0') as u32
}

/// Julian date of 1 January 0h UTC of year `y`.
fn jd_of_year_start(y: i32) -> f64 {
    let y = y as f64;
    367.0 * y - cfn::floor(7.0 * y / 4.0) + cfn::floor(275.0 / 9.0) + 1.0 + 1721013.5
}

/// Parses line 1 and line 2 (blanks around them are ignored) and the optional name. `name` is trimmed, a "0 " 3LE prefix removed. `name_base` is where `name`
/// starts in the buffer the caller maps the result's `name` range onto (0 when the caller passes the name text itself).
pub fn parse(a: &[u8], b: &[u8], name: &[u8], name_base: usize) -> Tle {
    let mut t = Tle { status: 1, ..Tle::default() };
    let (a0, a1) = trim(a);
    let (b0, b1) = trim(b);
    let l1 = &a[a0..a1];
    let l2 = &b[b0..b1];
    if l1.len() < 64 || l2.len() < 63 || l1[0] != b'1' || l2[0] != b'2' || !checksum_ok(l1) || !checksum_ok(l2) {
        return t;
    }
    let (norad, yr, day, bstar) = match (num(l1, 2, 5), num(l1, 18, 2), num(l1, 20, 12), exp_field(l1, 53, 8)) {
        (Some(n), Some(y), Some(d), Some(b)) => (n, y, d, b),
        _ => return t,
    };
    let (inc, raan, argp, ma, mm) = match (num(l2, 8, 8), num(l2, 17, 8), num(l2, 34, 8), num(l2, 43, 8), num(l2, 52, 11)) {
        (Some(a), Some(b), Some(c), Some(d), Some(e)) => (a, b, c, d, e),
        _ => return t,
    };
    let ecc = match num(l2, 26, 7) {
        Some(e) => e,
        None => return t,
    };
    match num(l2, 2, 5) {
        Some(n2) if n2 == norad => {}
        _ => return t,
    }
    let yr_i = yr as i32;
    let year = if yr_i < 57 { 2000 + yr_i } else { 1900 + yr_i };
    let (n0, n1) = trim(name);
    let mut nm = (n0, n1);
    if nm.1 - nm.0 > 2 && name[nm.0] == b'0' && name[nm.0 + 1] == b' ' {
        let (c0, c1) = trim(&name[nm.0 + 2..nm.1]);
        nm = (nm.0 + 2 + c0, nm.0 + 2 + c1);
    }
    t.norad = norad as i32;
    t.epoch_jd = jd_of_year_start(year) + day - 1.0;
    t.bstar = bstar;
    t.incl_deg = inc;
    t.raan_deg = raan;
    t.ecc = ecc * 1e-7;
    t.argp_deg = argp;
    t.mean_anom_deg = ma;
    t.mean_motion = mm;
    if nm.0 == nm.1 {
        t.name_is_norad = 1;
    } else {
        t.name = Span { off: (name_base + nm.0) as u32, len: (nm.1 - nm.0) as u32 };
    }
    t.status = if mm > 0.0 && t.ecc < 1.0 { 0 } else { 2 };
    t
}

/// Scans a text for element sets: lines are trimmed, blank lines dropped; a line starting with `1` followed by one starting with `2` is parsed (the line before
/// them is the name unless it starts with `1` or `2`); on success both lines are used up. Fills `out` with the parsed sets (status 0 only) and returns how many
/// there are in all (more than `out.len()` means the caller's array was too short).
pub fn scan_text(text: &[u8], out: &mut [Tle]) -> usize {
    // line ranges, built on the fly: previous line (name candidate), current line (candidate line 1), next
    let mut n = 0usize;
    let mut pos = 0usize;
    // trimmed non-empty lines as (start, end); iterate with a small window instead of storing them all
    let next_line = |pos: &mut usize| -> Option<(usize, usize)> {
        while *pos <= text.len() {
            if *pos == text.len() {
                return None;
            }
            let start = *pos;
            let mut e = start;
            while e < text.len() && text[e] != b'\n' {
                e += 1;
            }
            *pos = e + 1;
            let (a, b) = trim(&text[start..e]);
            if a < b {
                return Some((start + a, start + b));
            }
        }
        None
    };
    let mut prev: Option<(usize, usize)> = None; // line i-1
    let mut cur = next_line(&mut pos);
    while let Some(c) = cur {
        let nxt = next_line(&mut pos);
        let n2 = match nxt {
            Some(x) => x,
            None => break,
        };
        if text[c.0] == b'1' && text[n2.0] == b'2' {
            let name_line = match prev {
                Some(p) if text[p.0] != b'1' && text[p.0] != b'2' => p,
                _ => (0, 0),
            };
            let t = parse(&text[c.0..c.1], &text[n2.0..n2.1], &text[name_line.0..name_line.1], name_line.0);
            if t.status == 0 {
                if n < out.len() {
                    out[n] = t;
                }
                n += 1;
                // both lines are used up: the next candidate is the line after n2, whose predecessor is n2 itself
                prev = Some(n2);
                cur = next_line(&mut pos);
                continue;
            }
        }
        prev = Some(c);
        cur = Some(n2);
    }
    n
}
