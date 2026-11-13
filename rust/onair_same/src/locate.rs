// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Table lookups and the words for one location code `PSSCCC` (47 CFR 11.31 (c)): partial-county digit, state / province, county.
use crate::tables::{Named, Region, CANADA, EVENTS, ORIGINATORS, PCA, US};
use crate::text::{strip_nul, Buf};

/// Which table the two-digit state part is looked up in (the user's choice; the codes 10 - 62 overlap).
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Country {
    Us,
    Canada,
}

impl Country {
    /// 1 is Canada, anything else the US (the C++ enum has only those two).
    pub fn from_i32(v: i32) -> Country {
        if v == 1 {
            Country::Canada
        } else {
            Country::Us
        }
    }
}

fn find<'a>(t: &'a [Named], code: &[u8]) -> Option<&'a Named> {
    t.iter().find(|n| n.code.as_bytes() == code)
}

/// Originator name (47 CFR 11.31 (d)); "" when unknown. The result is NUL-terminated.
pub fn originator_name(code: &[u8]) -> &'static str {
    find(ORIGINATORS, code).map_or("\0", |n| n.name)
}

/// Event name (47 CFR 11.31 (e)); "" when unknown. NUL-terminated.
pub fn event_name(code: &[u8]) -> &'static str {
    find(EVENTS, code).map_or("\0", |n| n.name)
}

/// Event kind: from the table, else from the last letter of a code the table does not have (W warning, A watch, E emergency,
/// S statement, T test; 47 CFR 11.31 (e) names every code that way). NUL-terminated, "" when unknown.
pub fn event_kind(code: &[u8]) -> &'static str {
    if let Some(n) = find(EVENTS, code) {
        return n.kind;
    }
    if code.len() == 3 {
        match code[2] {
            b'W' => return "warning\0",
            b'A' => return "watch\0",
            b'E' => return "emergency\0",
            b'S' => return "statement\0",
            b'T' => return "test\0",
            _ => {}
        }
    }
    "\0"
}

/// Name of the Partial County Alerting digit (1 - 9); "" for 0 and out of range. NUL-terminated.
pub fn pca_name(d: i32) -> &'static str {
    if (1..=9).contains(&d) {
        PCA[d as usize]
    } else {
        "\0"
    }
}

/// The region of a two-digit code in the table of the country.
pub fn region(fips: i32, c: Country) -> Option<&'static Region> {
    let t: &'static [Region] = if c == Country::Canada { CANADA } else { US };
    t.iter().find(|r| r.fips == fips)
}

/// A character that may appear in a SAME message: digits, capitals, `+`, `-`, `/` and space (47 CFR 11.31 (a)).
pub fn valid_char(c: u8) -> bool {
    c.is_ascii_digit() || c.is_ascii_uppercase() || c == b'+' || c == b'-' || c == b'/' || c == b' '
}

/// True when `s[pos..pos + n]` exists and is all digits.
pub fn digits(s: &[u8], pos: usize, n: usize) -> bool {
    pos.checked_add(n).is_some_and(|e| e <= s.len()) && s[pos..pos + n].iter().all(u8::is_ascii_digit)
}

/// The number in `n` digits at `pos` (the caller has checked `digits`).
pub fn num(s: &[u8], pos: usize, n: usize) -> i32 {
    s[pos..pos + n].iter().fold(0i32, |v, &c| v * 10 + (c - b'0') as i32)
}

/// What [`locate`] found besides the text.
pub struct Loc {
    /// The code was six digits.
    pub ok: bool,
    pub pca: i32,
    pub state: i32,
    pub county: i32,
    pub region: Option<&'static Region>,
}

/// One location code in words, like "Northwest part of county FIPS 201, Texas". `county_name` is the user's county table entry (empty when
/// none). A code that is not six digits is written back unchanged. County names are never built in: a wrong name is worse than none.
pub fn locate(code: &[u8], country: Country, county_name: &[u8], out: &mut Buf) -> Loc {
    let mut l = Loc { ok: false, pca: 0, state: 0, county: 0, region: None };
    if code.len() != 6 || !digits(code, 0, 6) {
        out.bytes(code);
        return l;
    }
    l.ok = true;
    l.pca = num(code, 0, 1);
    l.state = num(code, 1, 2);
    l.county = num(code, 3, 3);
    l.region = region(l.state, country);
    let r = l.region;
    let start = out.len();
    if l.pca != 0 {
        out.cstr(pca_name(l.pca));
        out.str(" part of ");
    }
    if l.county == 0 {
        if l.pca == 0 {
            out.str("all of ");
        }
        match r {
            Some(r) => out.cstr(r.name),
            None => {
                out.str("state code ");
                out.dec(l.state as i64, 1);
            }
        }
    } else if !county_name.is_empty() {
        out.bytes(county_name);
        if let Some(r) = r {
            out.str(", ");
            out.cstr(if strip_nul(r.abbr).is_empty() { r.name } else { r.abbr });
        }
    } else {
        out.str("county FIPS ");
        out.dec(l.county as i64, 3);
        match r {
            Some(r) => {
                out.str(", ");
                out.cstr(r.name);
            }
            None => {
                out.str(" (state code ");
                out.dec(l.state as i64, 1);
                out.byte(b')');
            }
        }
    }
    if out.len() > start {
        if let Some(b) = out.first_mut() {
            *b = b.to_ascii_uppercase();
        }
    }
    l
}
