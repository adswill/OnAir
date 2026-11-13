// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_files` (core/include/dect2/rs/files.h). Every export is `onair_files_*`; pointers come as (pointer, length) pairs and the caller owns all buffers.
//! Text writers return the length the text has (also when the buffer was too short: nothing is written past it), `usize::MAX` marks a failure. Null pointers are
//! empty input / failure, never a crash.
//!
//! This is also where the C library functions live that `no_std` Rust has no bit-identical equivalent of (`cfn`: `pow`, `floor`, `snprintf` for `%g` / `%f`).
#![allow(clippy::missing_safety_doc)]

use crate::sigmf::{self, SigmfMeta};
use crate::sqlite::{self, Cell, Col, DbHeader};
use crate::tle::{self, Tle};
use crate::wav::{self, WavFmt};
use crate::{container, csv, json};

const FAIL: usize = usize::MAX;

/// C library functions with results the reference (C++) gets from the same calls.
pub(crate) mod cfn {
    use core::ffi::{c_char, c_int};
    mod c {
        use core::ffi::{c_char, c_int};
        extern "C" {
            pub fn pow(x: f64, y: f64) -> f64;
            pub fn floor(x: f64) -> f64;
            pub fn snprintf(s: *mut c_char, n: usize, fmt: *const c_char, ...) -> c_int;
        }
    }
    /// `pow(x, y)`.
    pub fn pow(x: f64, y: f64) -> f64 {
        // SAFETY: a pure C library function of two doubles.
        unsafe { c::pow(x, y) }
    }
    /// `floor(x)`.
    pub fn floor(x: f64) -> f64 {
        // SAFETY: a pure C library function of one double.
        unsafe { c::floor(x) }
    }
    fn fmt(f: &[u8], v: f64, out: &mut [u8; 64]) -> usize {
        // SAFETY: `out` is 64 writable bytes, `f` is NUL terminated with exactly one double conversion, one double is passed.
        let n: c_int = unsafe { c::snprintf(out.as_mut_ptr() as *mut c_char, out.len(), f.as_ptr() as *const c_char, v) };
        if n < 0 {
            0
        } else {
            (n as usize).min(out.len() - 1)
        }
    }
    /// `%.9g` or `%.10g` of `v` (other precisions print as 10).
    pub fn format_g(prec: u8, v: f64, out: &mut [u8; 64]) -> usize {
        fmt(if prec == 9 { b"%.9g\0" } else { b"%.10g\0" }, v, out)
    }
    /// `%.1f` (precision 1) or `%.0f` of `v`.
    pub fn format_f(prec: u8, v: f64, out: &mut [u8; 64]) -> usize {
        fmt(if prec == 0 { b"%.0f\0" } else { b"%.1f\0" }, v, out)
    }
}

unsafe fn rd<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(p, n)
    }
}

unsafe fn wr<'a, T>(p: *mut T, n: usize) -> &'a mut [T] {
    if p.is_null() || n == 0 {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(p, n)
    }
}

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_files_version() -> u32 {
    1
}

/// sizeof of the record structs in the order SigmfMeta, WavFmt, Tle, DbHeader, Col, Cell.
#[no_mangle]
pub unsafe extern "C" fn onair_files_struct_sizes(out: *mut usize) {
    if out.is_null() {
        return;
    }
    let v = [
        core::mem::size_of::<SigmfMeta>(),
        core::mem::size_of::<WavFmt>(),
        core::mem::size_of::<Tle>(),
        core::mem::size_of::<DbHeader>(),
        core::mem::size_of::<Col>(),
        core::mem::size_of::<Cell>(),
    ];
    for (i, x) in v.iter().enumerate() {
        *out.add(i) = *x;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------- CSV

/// Counts what `onair_files_csv_parse` produces: out[0] = records, out[1] = fields, out[2] = bytes of field text. False for a null `out`.
#[no_mangle]
pub unsafe extern "C" fn onair_files_csv_measure(text: *const u8, n: usize, out: *mut usize) -> bool {
    if out.is_null() {
        return false;
    }
    let s = csv::measure(rd(text, n));
    *out = s.records;
    *out.add(1) = s.fields;
    *out.add(2) = s.bytes;
    true
}

/// Parses CSV text: `data` gets the field bytes back to back, `ends[k]` the end offset of field k, `counts[r]` the field count of record r. False when an array is shorter
/// than `onair_files_csv_measure` says.
#[no_mangle]
pub unsafe extern "C" fn onair_files_csv_parse(
    text: *const u8,
    n: usize,
    data: *mut u8,
    n_data: usize,
    ends: *mut u32,
    n_ends: usize,
    counts: *mut u32,
    n_counts: usize,
) -> bool {
    csv::parse(rd(text, n), wr(data, n_data), wr(ends, n_ends), wr(counts, n_counts))
}

/// Length of the CSV form of a field (quoted when it holds a comma, quote, CR or LF).
#[no_mangle]
pub unsafe extern "C" fn onair_files_csv_quote_len(f: *const u8, n: usize) -> usize {
    csv::quote_len(rd(f, n))
}

/// Writes the CSV form of a field; returns its length or `usize::MAX` if `out` is too short.
#[no_mangle]
pub unsafe extern "C" fn onair_files_csv_quote(f: *const u8, n: usize, out: *mut u8, n_out: usize) -> usize {
    csv::quote(rd(f, n), wr(out, n_out))
}

/// Splits a `sep` separated list into trimmed non-empty items: `ends[2k]` / `ends[2k + 1]` = start / end of item k. Returns the item count; `usize::MAX` if `ends` is
/// too short (an empty `ends` only counts).
#[no_mangle]
pub unsafe extern "C" fn onair_files_split_list(s: *const u8, n: usize, sep: u32, ends: *mut u32, n_ends: usize) -> usize {
    let sep = sep as u8;
    csv::split_list(rd(s, n), sep, wr(ends, n_ends))
}

// ------------------------------------------------------------------------------------------------------------------------------------------------ JSON / SigMF

/// Resolves the escapes of a JSON string's raw bytes into `out`; returns the full length of the result (it can be longer than `n_out`, never longer than `n`).
#[no_mangle]
pub unsafe extern "C" fn onair_files_json_unescape(raw: *const u8, n: usize, out: *mut u8, n_out: usize) -> usize {
    json::unescape(rd(raw, n), wr(out, n_out))
}

/// Reads a `.sigmf-meta` text into `*out` (`status` tells whether it was usable, see `SigmfMeta`).
#[no_mangle]
pub unsafe extern "C" fn onair_files_sigmf_parse(text: *const u8, n: usize, out: *mut SigmfMeta) {
    if !out.is_null() {
        *out = sigmf::parse(rd(text, n));
    }
}

/// Maps a SigMF data type name to (format, big endian) (`FileFormat` numbering: 0 CS8, 1 CU8, 2 CF32, 3 CI16, 4 CU16). False for an unsupported name.
#[no_mangle]
pub unsafe extern "C" fn onair_files_sigmf_datatype(name: *const u8, n: usize, fmt: *mut i32, big_endian: *mut i32) -> bool {
    match sigmf::datatype(rd(name, n)) {
        Some((f, be)) => {
            if !fmt.is_null() {
                *fmt = f;
            }
            if !big_endian.is_null() {
                *big_endian = be as i32;
            }
            true
        }
        None => false,
    }
}

/// The meta text of the test tool (`writeSigmfMeta`); returns its length (the part that fits is written).
#[no_mangle]
pub unsafe extern "C" fn onair_files_sigmf_write_tool(
    datatype: *const u8,
    n_dt: usize,
    rate: f64,
    center: f64,
    sample_start: u64,
    desc: *const u8,
    n_desc: usize,
    out: *mut u8,
    n_out: usize,
) -> usize {
    sigmf::write_tool_meta(rd(datatype, n_dt), rate, center, sample_start, rd(desc, n_desc), wr(out, n_out))
}

/// The meta text of the recorder (`sigmfMetaJson`); returns its length (the part that fits is written). Empty `dataset` / `datetime` / `desc` and a zero `sample_count`
/// are left out.
#[no_mangle]
pub unsafe extern "C" fn onair_files_sigmf_write_recorder(
    datatype: *const u8,
    n_dt: usize,
    rate: f64,
    center: f64,
    datetime: *const u8,
    n_datetime: usize,
    dataset: *const u8,
    n_dataset: usize,
    sample_count: u64,
    desc: *const u8,
    n_desc: usize,
    out: *mut u8,
    n_out: usize,
) -> usize {
    sigmf::write_recorder_meta(
        rd(datatype, n_dt),
        rate,
        center,
        rd(datetime, n_datetime),
        rd(dataset, n_dataset),
        sample_count,
        rd(desc, n_desc),
        wr(out, n_out),
    )
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------ WAV

/// Reads the body of a `fmt ` chunk of declared length `len`.
#[no_mangle]
pub unsafe extern "C" fn onair_files_wav_fmt(body: *const u8, n: usize, len: u32, out: *mut WavFmt) {
    if !out.is_null() {
        *out = wav::parse_fmt(rd(body, n), len);
    }
}

/// Chooses the sample format: returns 0 and sets `*format` (`FileFormat`: 1 CU8, 2 CF32, 3 CI16), 1 when the file does not have two channels, 2 for an unsupported format.
#[no_mangle]
pub unsafe extern "C" fn onair_files_wav_classify(f: *const WavFmt, format: *mut i32) -> u32 {
    if f.is_null() {
        return 2;
    }
    let (code, fm) = wav::classify(&*f);
    if code == 0 && !format.is_null() {
        *format = fm;
    }
    code
}

/// Absolute end of the sample data (0 = until the end of the file).
#[no_mangle]
pub extern "C" fn onair_files_wav_data_end(offset: u64, len: u32, file_size: u64) -> u64 {
    wav::data_end(offset, len, file_size)
}

/// Centre frequency in Hz named in a file name ("..._100.000MHz_..."), 0 if none.
#[no_mangle]
pub unsafe extern "C" fn onair_files_wav_center_from_name(name: *const u8, n: usize) -> f64 {
    wav::center_from_name(rd(name, n))
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------ TLE

/// Parses one element set from its two lines and an optional name; the name range in the result is relative to `name`. `status` 0 = ok, 1 = not a TLE, 2 = unusable orbit.
#[no_mangle]
pub unsafe extern "C" fn onair_files_tle_parse(
    l1: *const u8,
    n1: usize,
    l2: *const u8,
    n2: usize,
    name: *const u8,
    n_name: usize,
    out: *mut Tle,
) {
    if !out.is_null() {
        *out = tle::parse(rd(l1, n1), rd(l2, n2), rd(name, n_name), 0);
    }
}

/// Finds all element sets in a text; the name ranges are relative to `text`. Returns the number found (more than `cap` means `out` was too short).
#[no_mangle]
pub unsafe extern "C" fn onair_files_tle_scan(text: *const u8, n: usize, out: *mut Tle, cap: usize) -> usize {
    tle::scan_text(rd(text, n), wr(out, cap))
}

// --------------------------------------------------------------------------------------------------------------------------------------------------- SQLite

/// Reads the 100 byte database header (`status` in the result).
#[no_mangle]
pub unsafe extern "C" fn onair_files_sqlite_header(h: *const u8, n: usize, out: *mut DbHeader) {
    if !out.is_null() {
        *out = sqlite::db_header(rd(h, n));
    }
}

/// Variable-length integer: returns the bytes used (1 to 9) and sets `*v`, 0 when `p` ends first.
#[no_mangle]
pub unsafe extern "C" fn onair_files_sqlite_varint(p: *const u8, n: usize, v: *mut u64) -> i32 {
    match sqlite::varint(rd(p, n)) {
        Some((x, k)) => {
            if !v.is_null() {
                *v = x;
            }
            k as i32
        }
        None => 0,
    }
}

/// Decodes a record into `cols` (capacity `cap`); returns the column count (more than `cap` = too short) or `usize::MAX` for a damaged record.
#[no_mangle]
pub unsafe extern "C" fn onair_files_sqlite_record(r: *const u8, n: usize, cols: *mut Col, cap: usize) -> usize {
    sqlite::record(rd(r, n), wr(cols, cap)).unwrap_or(FAIL)
}

/// Payload split: 0 = rejected, 1 = the whole payload is on the page (`*local` = total), 2 = `*local` bytes on the page and the rest on overflow pages.
#[no_mangle]
pub unsafe extern "C" fn onair_files_sqlite_payload_local(total: u64, usable: u64, index_tree: u32, local: *mut u64) -> i32 {
    let index_tree = index_tree != 0;
    match sqlite::payload_local(total, usable, index_tree) {
        Some((l, ov)) => {
            if !local.is_null() {
                *local = l;
            }
            if ov {
                2
            } else {
                1
            }
        }
        None => 0,
    }
}

/// Page header at `base`: out[0] = page type, out[1] = cell count, out[2] = right-most child. False when the page is too short.
#[no_mangle]
pub unsafe extern "C" fn onair_files_sqlite_page_header(pg: *const u8, n: usize, base: usize, out: *mut u32) -> bool {
    match sqlite::page_header(rd(pg, n), base) {
        Some((t, c, r)) if !out.is_null() => {
            *out = t as u32;
            *out.add(1) = c;
            *out.add(2) = r;
            true
        }
        _ => false,
    }
}

/// Decodes cell `i` of a page of kind `kind` (13, 5, 6 = 5 with key, 10, 2) whose header is at `base`. False for a damaged cell.
#[no_mangle]
pub unsafe extern "C" fn onair_files_sqlite_cell(
    pg: *const u8,
    n: usize,
    kind: u32,
    base: usize,
    i: usize,
    cells: usize,
    out: *mut Cell,
) -> bool {
    let kind = kind as u8;
    match sqlite::cell(rd(pg, n), kind, base, i, cells) {
        Some(c) if !out.is_null() => {
            *out = c;
            true
        }
        _ => false,
    }
}

/// MBTiles row of the XYZ tile (z, x, y): false outside the pyramid.
#[no_mangle]
pub unsafe extern "C" fn onair_files_sqlite_tile_row(z: i32, x: i32, y: i32, row: *mut i64) -> bool {
    match sqlite::tile_row(z, x, y) {
        Some(r) if !row.is_null() => {
            *row = r;
            true
        }
        _ => false,
    }
}

/// True for PNG or JPEG data.
#[no_mangle]
pub unsafe extern "C" fn onair_files_sqlite_is_raster(d: *const u8, n: usize) -> bool {
    sqlite::is_raster(rd(d, n))
}

/// True for PNG data.
#[no_mangle]
pub unsafe extern "C" fn onair_files_sqlite_is_png(d: *const u8, n: usize) -> bool {
    sqlite::is_png(rd(d, n))
}

/// Checks the column order of the `tiles` CREATE statement. `scratch` (at least `n` bytes) receives the squashed SQL; returns its length or `usize::MAX` when the layout is
/// not the standard one.
#[no_mangle]
pub unsafe extern "C" fn onair_files_sqlite_tiles_schema(sql: *const u8, n: usize, scratch: *mut u8, n_scratch: usize) -> usize {
    if n_scratch < n {
        return FAIL;
    }
    sqlite::tiles_schema_ok(rd(sql, n), wr(scratch, n_scratch)).unwrap_or(FAIL)
}

/// Whether an index (its SQL, possibly empty) covers (zoom_level, tile_column, tile_row); `tiles_squashed` is the squashed table SQL from `..._tiles_schema`.
#[no_mangle]
pub unsafe extern "C" fn onair_files_sqlite_index_covers(
    sql: *const u8,
    n: usize,
    scratch: *mut u8,
    n_scratch: usize,
    tiles_squashed: *const u8,
    n_tiles: usize,
) -> bool {
    n_scratch >= n && sqlite::index_covers(rd(sql, n), wr(scratch, n_scratch), rd(tiles_squashed, n_tiles))
}

// ----------------------------------------------------------------------------------------------------------------------------------------- results container

/// FNV-1a 64 bit.
#[no_mangle]
pub unsafe extern "C" fn onair_files_fnv1a(p: *const u8, n: usize) -> u64 {
    container::fnv1a(rd(p, n))
}

/// Signature hash of a signature text (FNV-1a xor the container format number).
#[no_mangle]
pub unsafe extern "C" fn onair_files_signature_hash(sig: *const u8, n: usize) -> u64 {
    container::signature_hash(rd(sig, n))
}

/// Checks the container header of a blob against the expected hash; returns the payload offset or `usize::MAX`.
#[no_mangle]
pub unsafe extern "C" fn onair_files_container_check(blob: *const u8, n: usize, hash: u64) -> usize {
    container::check_header(rd(blob, n), hash).unwrap_or(FAIL)
}

/// Writes the 16 byte container header for a hash. False if `out` is shorter.
#[no_mangle]
pub unsafe extern "C" fn onair_files_container_header(hash: u64, out: *mut u8, n_out: usize) -> bool {
    container::write_header(hash, wr(out, n_out))
}
