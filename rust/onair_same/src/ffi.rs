// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_same` (core/include/dect2/rs/same.h). Every export is `onair_same_*`; pointers come as (pointer, length) pairs and the caller
//! owns all buffers. Functions that write return the length written, or `usize::MAX` when the buffer is too small. Strings handed out as
//! `const char*` point at static NUL-terminated tables ("" when unknown).
#![allow(clippy::missing_safety_doc)]

use crate::locate::{self, Country};
use crate::text::Buf;
use crate::{county, parse, time};

const FAIL: usize = usize::MAX;

unsafe fn rd<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(p, n)
    }
}
unsafe fn wr<'a>(p: *mut u8, n: usize) -> &'a mut [u8] {
    if p.is_null() || n == 0 {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(p, n)
    }
}

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_same_version() -> u32 {
    1
}

/// sizeof(OnairSameMsg) and sizeof(OnairSameLoc): the C++ side checks them against its own structs when the program starts.
#[no_mangle]
pub unsafe extern "C" fn onair_same_struct_sizes(msg: *mut usize, loc: *mut usize) {
    if !msg.is_null() {
        *msg = core::mem::size_of::<OnairSameMsg>();
    }
    if !loc.is_null() {
        *loc = core::mem::size_of::<OnairSameLoc>();
    }
}

#[no_mangle]
pub unsafe extern "C" fn onair_same_originator_name(code: *const u8, n: usize) -> *const u8 {
    locate::originator_name(rd(code, n)).as_ptr()
}
#[no_mangle]
pub unsafe extern "C" fn onair_same_event_name(code: *const u8, n: usize) -> *const u8 {
    locate::event_name(rd(code, n)).as_ptr()
}
#[no_mangle]
pub unsafe extern "C" fn onair_same_event_kind(code: *const u8, n: usize) -> *const u8 {
    locate::event_kind(rd(code, n)).as_ptr()
}
#[no_mangle]
pub extern "C" fn onair_same_pca_name(digit: i32) -> *const u8 {
    locate::pca_name(digit).as_ptr()
}
#[no_mangle]
pub extern "C" fn onair_same_state_name(fips: i32, country: i32) -> *const u8 {
    locate::region(fips, Country::from_i32(country)).map_or("\0", |r| r.name).as_ptr()
}
#[no_mangle]
pub extern "C" fn onair_same_state_abbr(fips: i32, country: i32) -> *const u8 {
    locate::region(fips, Country::from_i32(country)).map_or("\0", |r| r.abbr).as_ptr()
}
/// True and the centre when the region exists and claims one.
#[no_mangle]
pub unsafe extern "C" fn onair_same_state_centre(fips: i32, country: i32, lat: *mut f64, lon: *mut f64) -> bool {
    match locate::region(fips, Country::from_i32(country)) {
        Some(r) if r.lat != 0.0 || r.lon != 0.0 => {
            if !lat.is_null() {
                *lat = r.lat;
            }
            if !lon.is_null() {
                *lon = r.lon;
            }
            true
        }
        _ => false,
    }
}
#[no_mangle]
pub extern "C" fn onair_same_valid_char(c: u32) -> bool {
    // u32, not u8: GCC does not widen a narrow argument and LLVM relies on it (differs on x86-64 Linux)
    c <= 0xff && locate::valid_char(c as u8)
}
/// The message cut to what the format says: writes the start offset to `start` and returns the length (0: neither "ZCZC" nor "NNNN" found).
#[no_mangle]
pub unsafe extern "C" fn onair_same_trim_message(b: *const u8, n: usize, start: *mut usize) -> usize {
    let (s, l) = parse::trim_message(rd(b, n));
    if !start.is_null() {
        *start = s;
    }
    l
}

/// Location facts (the words come from `onair_same_locate`). `region_known` says the state code is in the table of the country.
#[repr(C)]
pub struct OnairSameLoc {
    pub ok: u8,
    pub region_known: u8,
    pub has_pos: u8,
    pub pca: i32,
    pub state: i32,
    pub county: i32,
    pub lat: f64,
    pub lon: f64,
}

/// One location code in words. `county_name` is the user's table entry for the code ("" when none). Returns the text length or SIZE_MAX.
#[no_mangle]
pub unsafe extern "C" fn onair_same_locate(
    code: *const u8,
    n: usize,
    country: i32,
    county_name: *const u8,
    county_len: usize,
    info: *mut OnairSameLoc,
    out: *mut u8,
    cap: usize,
) -> usize {
    let mut b = Buf::new(wr(out, cap));
    let l = locate::locate(rd(code, n), Country::from_i32(country), rd(county_name, county_len), &mut b);
    if b.overflow {
        return FAIL;
    }
    if !info.is_null() {
        let (has, lat, lon) = l.region.map_or((false, 0.0, 0.0), |r| (r.lat != 0.0 || r.lon != 0.0, r.lat, r.lon));
        *info = OnairSameLoc {
            ok: l.ok as u8,
            region_known: l.region.is_some() as u8,
            has_pos: has as u8,
            pca: l.pca,
            state: l.state,
            county: l.county,
            lat,
            lon,
        };
    }
    b.len()
}

/// The parsed header; offsets refer to the text that was parsed. Strings are NUL-terminated static tables.
#[repr(C)]
pub struct OnairSameMsg {
    pub valid: u8,
    pub eom: u8,
    pub org_set: u8,
    pub event_set: u8,
    pub call_set: u8,
    pub nloc: u32,
    pub purge_min: i32,
    pub issue_day: i32,
    pub issue_hour: i32,
    pub issue_min: i32,
    pub call_start: usize,
    pub call_len: usize,
    pub err: *const u8,
    pub org_name: *const u8,
    pub event_name: *const u8,
    pub event_kind: *const u8,
}

/// Parses and checks "ZCZC-..." or "NNNN".
#[no_mangle]
pub unsafe extern "C" fn onair_same_parse(b: *const u8, n: usize, m: *mut OnairSameMsg) {
    if m.is_null() {
        return;
    }
    let p = parse::parse(rd(b, n));
    *m = OnairSameMsg {
        valid: p.valid as u8,
        eom: p.eom as u8,
        org_set: p.org_set as u8,
        event_set: p.event_set as u8,
        call_set: p.call_set as u8,
        nloc: p.nloc as u32,
        purge_min: p.purge_min,
        issue_day: p.issue_day,
        issue_hour: p.issue_hour,
        issue_min: p.issue_min,
        call_start: p.call_start,
        call_len: p.call_len,
        err: p.err.as_ptr(),
        org_name: p.org_name.as_ptr(),
        event_name: p.event_name.as_ptr(),
        event_kind: p.event_kind.as_ptr(),
    };
}

#[no_mangle]
pub extern "C" fn onair_same_issue_epoch(year: i32, day: i32, hour: i32, minute: i32) -> i64 {
    time::issue_epoch(year, day, hour, minute)
}
/// "May 03 14:30 UTC" (empty when out of range).
#[no_mangle]
pub unsafe extern "C" fn onair_same_issue_text(year: i32, day: i32, hour: i32, minute: i32, out: *mut u8, cap: usize) -> usize {
    let mut b = Buf::new(wr(out, cap));
    time::issue_text(year, day, hour, minute, &mut b);
    if b.overflow {
        FAIL
    } else {
        b.len()
    }
}
/// "1 h 30 min".
#[no_mangle]
pub unsafe extern "C" fn onair_same_purge_text(minutes: i32, out: *mut u8, cap: usize) -> usize {
    let mut b = Buf::new(wr(out, cap));
    time::purge_text(minutes, &mut b);
    if b.overflow {
        FAIL
    } else {
        b.len()
    }
}

/// County CSV text to records (key u32 LE, name length u32 LE, name); out >= 2 * n + 16 and scratch >= n are always enough.
#[no_mangle]
pub unsafe extern "C" fn onair_same_read_county_csv(
    t: *const u8,
    n: usize,
    out: *mut u8,
    cap: usize,
    scratch: *mut u8,
    scap: usize,
) -> usize {
    county::read_csv(rd(t, n), wr(out, cap), wr(scratch, scap)).unwrap_or(FAIL)
}
