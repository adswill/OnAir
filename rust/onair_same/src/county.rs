// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The user's county table: reads the CSV text into `(SSCCC key, name)` records. Accepted lines are `SSCCC,Name`, `SSSCCC...` (a full SAME code,
//! the PCA digit ignored), `SS,CCC,Name` and the Census file `national_county.txt` (`AL,01,001,Autauga County,H1`); anything else (a header,
//! blank lines, county 000 = the whole state) is skipped. Quotes group a field, are dropped, and `\r` is ignored.
use crate::text::Buf;

fn is_space(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r')
}

fn all_digits(s: &[u8], n: usize) -> bool {
    s.len() == n && s.iter().all(u8::is_ascii_digit)
}

fn atoi(s: &[u8]) -> i32 {
    s.iter().fold(0i32, |v, &c| v.wrapping_mul(10).wrapping_add((c - b'0') as i32))
}

/// One line split into its first four fields (trimmed) and the number of fields; `scratch` receives the line without quotes and `\r`.
struct Fields {
    n: usize,
    r: [(usize, usize); 4],
}

fn split(line: &[u8], scratch: &mut [u8]) -> Option<Fields> {
    let mut f = Fields { n: 0, r: [(0, 0); 4] };
    let (mut len, mut start, mut quoted) = (0usize, 0usize, false);
    let close = |f: &mut Fields, start: usize, end: usize, scratch: &[u8]| {
        let (mut a, mut b) = (start, end);
        while a < b && is_space(scratch[a]) {
            a += 1;
        }
        while b > a && is_space(scratch[b - 1]) {
            b -= 1;
        }
        if f.n < 4 {
            f.r[f.n] = (a, b);
        }
        f.n += 1;
    };
    for &ch in line {
        if ch == b'"' {
            quoted = !quoted;
        } else if ch == b',' && !quoted {
            close(&mut f, start, len, scratch);
            start = len;
        } else if ch != b'\r' {
            *scratch.get_mut(len)? = ch;
            len += 1;
        }
    }
    close(&mut f, start, len, scratch);
    Some(f)
}

/// Reads the table. Records are written to `out` as `key: u32 LE (state * 1000 + county)`, `len: u32 LE`, the name bytes; `scratch` must be at least
/// as long as the longest line. Returns the bytes written, or `None` when `out` or `scratch` is too small. Later lines replace earlier ones for
/// the same key on the C++ side (a map).
pub fn read_csv(text: &[u8], out: &mut [u8], scratch: &mut [u8]) -> Option<usize> {
    let mut o = Buf::new(out);
    for line in text.split(|&c| c == b'\n') {
        let f = split(line, scratch)?;
        let fld = |k: usize| -> &[u8] { &scratch[f.r[k].0..f.r[k].1] };
        let (st, co, name): (i32, i32, &[u8]);
        if f.n >= 4 && fld(0).len() == 2 && !fld(0)[0].is_ascii_digit() && all_digits(fld(1), 2) && all_digits(fld(2), 3) {
            (st, co, name) = (atoi(fld(1)), atoi(fld(2)), fld(3)); // Census national_county.txt
        } else if f.n >= 3 && all_digits(fld(0), 2) && all_digits(fld(1), 3) {
            (st, co, name) = (atoi(fld(0)), atoi(fld(1)), fld(2));
        } else if f.n >= 2 && all_digits(fld(0), 5) {
            (st, co, name) = (atoi(&fld(0)[..2]), atoi(&fld(0)[2..]), fld(1));
        } else if f.n >= 2 && all_digits(fld(0), 6) {
            (st, co, name) = (atoi(&fld(0)[1..3]), atoi(&fld(0)[3..]), fld(1)); // a full SAME code: the PCA digit is ignored
        } else {
            continue;
        }
        if co <= 0 || name.is_empty() {
            continue; // header, blank or other lines; county 000 is the state
        }
        o.bytes(&((st * 1000 + co) as u32).to_le_bytes());
        o.bytes(&(name.len() as u32).to_le_bytes());
        o.bytes(name);
        if o.overflow {
            return None;
        }
    }
    Some(o.len())
}
