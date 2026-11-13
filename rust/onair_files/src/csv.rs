// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Comma separated values as the OnAir channel list files use them (`scripts/ingest.py` writes them, RFC 4180 style): fields separated by `,`, records by
//! `\n`, a field may be quoted with `"` (then it can hold `,` and line breaks and a doubled `""` is one quote), `\r` outside quotes is ignored, blank lines are skipped.
//!
//! The parser works in two passes over the caller's text without allocating: `measure` counts records, fields and the bytes of unquoted field text, `parse` writes the
//! field bytes one after another into a caller buffer plus, for each field, its end offset in that buffer, and for each record its number of fields.

/// Sizes of a parsed text.
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct CsvSize {
    /// Records (blank lines are not records).
    pub records: usize,
    /// Fields of all records.
    pub fields: usize,
    /// Bytes of all field texts after removing the quotes.
    pub bytes: usize,
}

/// The state machine shared by both passes; calls `field_byte` for every byte that belongs to a field and `end_field` / `end_record` at the boundaries.
fn walk(t: &[u8], mut byte: impl FnMut(u8), mut end_field: impl FnMut(), mut end_record: impl FnMut(usize, bool)) {
    // `nf` = fields finished in the current record; `cur_len` = bytes in the field being built; `any` as in the reference
    let mut q = false;
    let mut any = false;
    let mut nf = 0usize;
    let mut cur_len = 0usize;
    let mut i = 0usize;
    while i < t.len() {
        let c = t[i];
        if q {
            if c == b'"' {
                if i + 1 < t.len() && t[i + 1] == b'"' {
                    byte(b'"');
                    cur_len += 1;
                    i += 1;
                } else {
                    q = false;
                }
            } else {
                byte(c);
                cur_len += 1;
            }
        } else if c == b'"' {
            q = true;
            any = true;
        } else if c == b',' {
            end_field();
            nf += 1;
            cur_len = 0;
            any = true;
        } else if c == b'\n' {
            end_field();
            nf += 1;
            // a record that is a single empty field is a blank line
            end_record(nf, nf == 1 && cur_len == 0);
            nf = 0;
            cur_len = 0;
            any = false;
        } else if c != b'\r' {
            byte(c);
            cur_len += 1;
            any = true;
        }
        i += 1;
    }
    if any || cur_len != 0 || nf != 0 {
        end_field();
        nf += 1;
        end_record(nf, nf == 1 && cur_len == 0);
    }
}

/// Counts what `parse` will produce.
pub fn measure(t: &[u8]) -> CsvSize {
    use core::cell::Cell;
    let s = (Cell::new(0usize), Cell::new(0usize), Cell::new(0usize));
    walk(
        t,
        |_| s.2.set(s.2.get() + 1),
        || {},
        |nf, blank| {
            if !blank {
                s.0.set(s.0.get() + 1);
                s.1.set(s.1.get() + nf);
            }
        },
    );
    CsvSize { records: s.0.get(), fields: s.1.get(), bytes: s.2.get() }
}

/// Parses `t`. `data` receives the field bytes of all non-blank records back to back, `ends[k]` the end offset (in `data`) of field k, `counts[r]` the number of fields
/// of record r. The slices must be at least as long as `measure` says; returns false when they are not.
pub fn parse(t: &[u8], data: &mut [u8], ends: &mut [u32], counts: &mut [u32]) -> bool {
    use core::cell::Cell;
    let size = measure(t);
    if data.len() < size.bytes || ends.len() < size.fields || counts.len() < size.records || size.bytes > u32::MAX as usize {
        return false;
    }
    // the field text of a blank record has no bytes, so `data` only ever holds bytes of kept records; the cursors live in cells because three closures share them
    let dpos = Cell::new(0usize);
    let epos = Cell::new(0usize);
    let rpos = Cell::new(0usize);
    let rec_start = Cell::new(0usize); // epos at the start of the current record
    let data_cell = core::cell::RefCell::new(data);
    let ends_cell = core::cell::RefCell::new(ends);
    let counts_cell = core::cell::RefCell::new(counts);
    walk(
        t,
        |b| {
            data_cell.borrow_mut()[dpos.get()] = b;
            dpos.set(dpos.get() + 1);
        },
        || {
            // the end entry of the field of a blank line is thrown away again, and may lie past the array (the measured size has no room for it)
            if let Some(slot) = ends_cell.borrow_mut().get_mut(epos.get()) {
                *slot = dpos.get() as u32;
            }
            epos.set(epos.get() + 1);
        },
        |nf, blank| {
            if blank {
                // forget the empty field just written
                epos.set(rec_start.get());
            } else {
                counts_cell.borrow_mut()[rpos.get()] = nf as u32;
                rpos.set(rpos.get() + 1);
                rec_start.set(epos.get());
            }
        },
    );
    true
}

/// Length of `quote(f)`: `f` itself, or `f` between quotes with every quote doubled when it holds `,`, `"`, CR or LF.
pub fn quote_len(f: &[u8]) -> usize {
    if !f.iter().any(|&c| matches!(c, b',' | b'"' | b'\r' | b'\n')) {
        return f.len();
    }
    2 + f.len() + f.iter().filter(|&&c| c == b'"').count()
}

/// Writes `quote(f)` into `out` (must be `quote_len(f)` bytes or more); returns the length, or `usize::MAX` when `out` is too short.
pub fn quote(f: &[u8], out: &mut [u8]) -> usize {
    let n = quote_len(f);
    if out.len() < n {
        return usize::MAX;
    }
    if n == f.len() {
        out[..n].copy_from_slice(f);
        return n;
    }
    let mut k = 0;
    out[k] = b'"';
    k += 1;
    for &c in f {
        if c == b'"' {
            out[k] = b'"';
            k += 1;
        }
        out[k] = c;
        k += 1;
    }
    out[k] = b'"';
    k + 1
}

/// Splits a `sep` separated list (services of a channel, "A|B|C") into trimmed non-empty items. `ends[k]` receives the (start, end) of item k in `s`
/// (start of item at `ends[2k]`, end at `ends[2k+1]`); returns the item count, or `usize::MAX` if `ends` is too short for them (pass an empty slice to count).
/// Trimming removes spaces, tabs and CR at both ends.
pub fn split_list(s: &[u8], sep: u8, ends: &mut [u32]) -> usize {
    let mut n = 0usize;
    let mut start = 0usize;
    let mut i = 0usize;
    let mut ok = true;
    while i <= s.len() {
        if i == s.len() || s[i] == sep {
            let (mut a, mut b) = (start, i);
            while a < b && matches!(s[a], b' ' | b'\t' | b'\r') {
                a += 1;
            }
            while b > a && matches!(s[b - 1], b' ' | b'\t' | b'\r') {
                b -= 1;
            }
            if a < b {
                if 2 * n + 1 < ends.len() {
                    ends[2 * n] = a as u32;
                    ends[2 * n + 1] = b as u32;
                } else if !ends.is_empty() {
                    ok = false;
                }
                n += 1;
            }
            start = i + 1;
        }
        i += 1;
    }
    if ok || ends.is_empty() {
        n
    } else {
        usize::MAX
    }
}
