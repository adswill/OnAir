// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! APRS positions (APRS101 chapters 8 and 9): uncompressed latitude / longitude, base-91 compressed positions, and Mic-E (chapter 10), where the
//! latitude is in the destination address and the longitude, speed and course are in the information field.
use crate::aprs::Info;
use crate::arena::{Arena, Span};
use crate::ffi::cfn;

fn all_digits(s: &[u8], a: usize, n: usize) -> bool {
    a + n <= s.len() && s[a..a + n].iter().all(|c| c.is_ascii_digit())
}

/// Integer value of up to nine decimal digits (all digits already checked).
fn int_of(s: &[u8]) -> f64 {
    let mut v = 0i32;
    for &c in s {
        v = v * 10 + i32::from(c - b'0');
    }
    f64::from(v)
}

/// "4903.50N" to degrees (spaces count as zero: position ambiguity). Needs 8 bytes.
pub fn parse_lat(s: &[u8]) -> Option<f64> {
    if s.len() < 8 {
        return None;
    }
    let mut t = [0u8; 8];
    for (d, &c) in t.iter_mut().zip(s) {
        *d = if c == b' ' { b'0' } else { c };
    }
    if !all_digits(&t, 0, 4) || t[4] != b'.' || !all_digits(&t, 5, 2) {
        return None;
    }
    let h = t[7];
    if h != b'N' && h != b'S' {
        return None;
    }
    // minutes "MM.hh" as the integer MMhh over 100: the same value atof gives
    let min = (int_of(&t[2..4]) * 100.0 + int_of(&t[5..7])) / 100.0;
    let mut v = int_of(&t[..2]) + min / 60.0;
    if v > 90.0 {
        return None;
    }
    if h == b'S' {
        v = -v;
    }
    Some(v)
}

/// "07201.50W" to degrees. Needs 9 bytes.
pub fn parse_lon(s: &[u8]) -> Option<f64> {
    if s.len() < 9 {
        return None;
    }
    let mut t = [0u8; 9];
    for (d, &c) in t.iter_mut().zip(s) {
        *d = if c == b' ' { b'0' } else { c };
    }
    if !all_digits(&t, 0, 5) || t[5] != b'.' || !all_digits(&t, 6, 2) {
        return None;
    }
    let h = t[8];
    if h != b'E' && h != b'W' {
        return None;
    }
    let min = (int_of(&t[3..5]) * 100.0 + int_of(&t[6..8])) / 100.0;
    let mut v = int_of(&t[..3]) + min / 60.0;
    if v > 180.0 {
        return None;
    }
    if h == b'W' {
        v = -v;
    }
    Some(v)
}

/// Base-91 number of `n` characters at `a` (each character minus 33).
pub fn base91(s: &[u8], a: usize, n: usize) -> i64 {
    let mut v = 0i64;
    for i in 0..n {
        v = v * 91 + (i64::from(s[a + i]) - 33);
    }
    v
}

/// The position part at the start of `s`: uncompressed (19 characters) or compressed (13). Fills position, symbol, altitude / course / speed in `o`.
/// Returns the characters used, or 0.
pub fn parse_pos(s: &[u8], o: &mut Info) -> usize {
    if s.is_empty() {
        return 0;
    }
    let f = s[0];
    if f.is_ascii_digit() || f == b' ' {
        if s.len() < 19 {
            return 0;
        }
        let (Some(la), Some(lo)) = (parse_lat(&s[..8]), parse_lon(&s[9..18])) else { return 0 };
        o.has_pos = true;
        o.lat = la;
        o.lon = lo;
        o.sym_table = s[8];
        o.sym_code = s[18];
        return 19;
    }
    if s.len() < 13 {
        return 0;
    }
    if !(f == b'/' || f == b'\\' || f.is_ascii_uppercase() || (b'a'..=b'j').contains(&f)) {
        return 0;
    }
    if s[1..=8].iter().any(|&c| !(33..=123).contains(&c)) {
        return 0;
    }
    o.has_pos = true;
    o.lat = 90.0 - base91(s, 1, 4) as f64 / 380926.0;
    o.lon = -180.0 + base91(s, 5, 4) as f64 / 190463.0;
    o.sym_table = s[0];
    o.sym_code = s[9];
    let c = i32::from(s[10]) - 33;
    let sp = i32::from(s[11]) - 33;
    let t = i32::from(s[12]) - 33;
    if c >= 0 && sp >= 0 {
        if (t & 0x18) == 0x10 {
            o.has_alt = true;
            o.alt_m = cfn::pow(1.002, f64::from(c * 91 + sp)) * 0.3048;
        } else if c < 90 {
            if c > 0 {
                o.has_course = true;
                o.course_deg = c * 4;
            }
            o.has_speed = true;
            o.speed_knots = cfn::pow(1.08, f64::from(sp)) - 1.0;
        }
    }
    13
}

/// Mic-E message names by the three standard message bits (APRS101 table on page 45).
const MIC_E_NAMES: [&str; 8] = ["Emergency", "Priority", "Special", "Committed", "Returning", "In Service", "En Route", "Off Duty"];

/// Decodes a Mic-E packet: `dest` is the destination callsign (latitude digits and flags), `info` the information field. Writes position, speed,
/// course, symbol, comment (arena), altitude and message name into `o`. False when it is not a valid Mic-E packet.
pub fn mic_e(dest: &[u8], info: &[u8], o: &mut Info, arena: &mut Arena) -> bool {
    if dest.len() < 6 || info.len() < 9 {
        return false;
    }
    let mut dig = [0i32; 6];
    let (mut std_bits, mut custom) = (0usize, 0);
    for i in 0..6 {
        let c = dest[i];
        let (mut bit, mut cust) = (false, false);
        let d = match c {
            b'0'..=b'9' => i32::from(c - b'0'),
            b'A'..=b'J' => {
                cust = true;
                i32::from(c - b'A')
            }
            b'K' => {
                cust = true;
                0
            }
            b'L' => 0,
            b'P'..=b'Y' => {
                bit = true;
                i32::from(c - b'P')
            }
            b'Z' => {
                bit = true;
                0
            }
            _ => return false,
        };
        dig[i] = d;
        if i < 3 {
            if cust {
                custom |= 4 >> i;
            }
            if bit {
                std_bits |= 4 >> i;
            }
        }
    }
    let high = |i: usize| (b'P'..=b'Z').contains(&dest[i]);
    let mut lat = f64::from(dig[0] * 10 + dig[1]) + f64::from(dig[2] * 1000 + dig[3] * 100 + dig[4] * 10 + dig[5]) / 6000.0;
    if !high(3) {
        lat = -lat;
    }
    let (offs, west) = (high(4), high(5));
    let (lo0, lo1, lo2) = (i32::from(info[1]), i32::from(info[2]), i32::from(info[3]));
    let mut d = lo0 - 28 + if offs { 100 } else { 0 };
    if (180..=189).contains(&d) {
        d -= 80;
    } else if (190..=199).contains(&d) {
        d -= 190;
    }
    let mut m = lo1 - 28;
    if m >= 60 {
        m -= 60;
    }
    let h = lo2 - 28;
    if !(0..=179).contains(&d) || !(0..=59).contains(&m) || !(0..=99).contains(&h) {
        return false;
    }
    let mut lon = f64::from(d) + (f64::from(m) + f64::from(h) / 100.0) / 60.0;
    if west {
        lon = -lon;
    }
    let sp = i32::from(info[4]) - 28;
    let dc = i32::from(info[5]) - 28;
    let se = i32::from(info[6]) - 28;
    let mut speed = sp * 10 + dc / 10;
    if speed >= 800 {
        speed -= 800;
    }
    let mut course = (dc % 10) * 100 + se;
    if course >= 400 {
        course -= 400;
    }
    o.has_pos = true;
    o.lat = lat;
    o.lon = lon;
    o.has_speed = true;
    o.speed_knots = f64::from(speed);
    if course > 0 {
        o.has_course = true;
        o.course_deg = if course == 360 { 0 } else { course };
    }
    o.sym_code = info[7];
    o.sym_table = info[8];
    let comment = &info[9..];
    // altitude: three base-91 characters and '}' near the start of the comment, metres above -10000
    let mut skip: Option<usize> = None;
    let mut i = 0;
    while i + 3 < comment.len() && i < 4 {
        if comment[i + 3] == b'}' && comment[i] >= 33 && comment[i + 1] >= 33 && comment[i + 2] >= 33 {
            o.has_alt = true;
            o.alt_m = base91(comment, i, 3) as f64 - 10000.0;
            skip = Some(i);
            break;
        }
        i += 1;
    }
    let m0 = arena.mark();
    match skip {
        Some(i) => {
            arena.put(&comment[..i]);
            arena.put(&comment[i + 4..]);
        }
        None => arena.put(comment),
    }
    o.comment = arena.since(m0);
    if custom == 0 && std_bits != 0 {
        o.mic_status = arena.push(MIC_E_NAMES[std_bits].as_bytes());
    } else {
        o.mic_status = Span::default();
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn lat_lon() {
        assert_eq!(parse_lat(b"4903.50N"), Some(49.0 + 3.5 / 60.0));
        assert_eq!(parse_lat(b"49  .  S"), Some(-49.0));
        assert_eq!(parse_lat(b"49 x.  S"), None);
        assert!(parse_lat(b"4900.00X").is_none());
        assert_eq!(parse_lon(b"07201.75W"), Some(-(72.0 + 1.75 / 60.0)));
        assert!(parse_lat(b"9100.00N").is_none());
    }
    #[test]
    fn base91_value() {
        assert_eq!(base91(b"!!!!", 0, 4), 0);
        assert_eq!(base91(b"\"!!!", 0, 4), 91 * 91 * 91);
    }
}
