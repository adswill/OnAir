// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_wisun` (core/include/dect2/rs/wisun.h). Every export is `onair_wisun_*`; pointers come as (pointer, length) pairs and the caller owns all
//! buffers. A null pointer with a length is read as an empty input. Name functions return pointers to static NUL-terminated text.
//!
//! This is also the only module with `unsafe`: besides the C ABI it holds `cfn`, the C library `snprintf("%.3f")` the explicit channel plan text is made with.
#![allow(clippy::missing_safety_doc)]

use crate::ie::Schedule;
use crate::mac::{self, Frame};
use crate::names;
use crate::phy::{self, Phr};
use core::ffi::c_char;
use onair_common::text::Buf;

/// C library `snprintf("%.Nf")` for the digits of numbers in text.
pub(crate) mod cfn {
    use core::ffi::{c_char, c_int};
    mod c {
        use core::ffi::{c_char, c_int};
        extern "C" {
            pub fn snprintf(s: *mut c_char, n: usize, fmt: *const c_char, ...) -> c_int;
        }
    }
    /// Writes `v` as C `%.{prec}f` (prec 0 to 3) into `out` (64 bytes); returns the length (at most 63).
    pub fn format_f(prec: u8, v: f64, out: &mut [u8; 64]) -> usize {
        const FMT: [&[u8]; 4] = [b"%.0f\0", b"%.1f\0", b"%.2f\0", b"%.3f\0"];
        let f = FMT[usize::from(prec.min(3))];
        // SAFETY: `out` is 64 writable bytes, `f` is NUL terminated with one double conversion, one double is passed.
        let n: c_int = unsafe { c::snprintf(out.as_mut_ptr() as *mut c_char, out.len(), f.as_ptr() as *const c_char, v) };
        if n < 0 {
            0
        } else {
            (n as usize).min(out.len() - 1)
        }
    }
}

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
pub extern "C" fn onair_wisun_version() -> u32 {
    2
}

/// `sizeof` of Frame, Schedule and Phr in this order (`out` has room for 3): the C++ side checks them at start-up.
#[no_mangle]
pub unsafe extern "C" fn onair_wisun_struct_sizes(out: *mut usize) {
    if !out.is_null() {
        *out = core::mem::size_of::<Frame>();
        *out.add(1) = core::mem::size_of::<Schedule>();
        *out.add(2) = core::mem::size_of::<Phr>();
    }
}

/// Splits a PHR value.
#[no_mangle]
pub unsafe extern "C" fn onair_wisun_parse_phr(v: u32, out: *mut Phr) {
    let v = v as u16;
    if !out.is_null() {
        *out = phy::parse_phr(v);
    }
}

/// Builds a PHR value.
#[no_mangle]
pub unsafe extern "C" fn onair_wisun_build_phr(p: *const Phr) -> u16 {
    if p.is_null() {
        0
    } else {
        phy::build_phr(&*p)
    }
}

/// PN9 whitening of `n` octets in place.
#[no_mangle]
pub unsafe extern "C" fn onair_wisun_whiten(d: *mut u8, n: usize) {
    phy::whiten(wr(d, n));
}

/// The 2-octet FCS (CRC-16/KERMIT).
#[no_mangle]
pub unsafe extern "C" fn onair_wisun_crc16(d: *const u8, n: usize) -> u16 {
    phy::crc16(rd(d, n))
}

/// The 4-octet FCS (CRC-32).
#[no_mangle]
pub unsafe extern "C" fn onair_wisun_crc32(d: *const u8, n: usize) -> u32 {
    phy::crc32(rd(d, n))
}

/// True when the FCS at the end of the `n` octet PSDU is right.
#[no_mangle]
pub unsafe extern "C" fn onair_wisun_fcs_ok(p: *const u8, n: usize, fcs2: u32) -> bool {
    let fcs2 = fcs2 != 0;
    phy::fcs_ok(rd(p, n), fcs2)
}

/// Parses an MPDU of `n` octets (FCS excluded) into `*out`.
#[no_mangle]
pub unsafe extern "C" fn onair_wisun_parse(d: *const u8, n: usize, out: *mut Frame) {
    if !out.is_null() {
        *out = mac::parse(rd(d, n));
    }
}

/// UTT frame type name ("PAN Advert", ...).
#[no_mangle]
pub extern "C" fn onair_wisun_utt_name(t: i32) -> *const c_char {
    names::utt_name_z(t).as_ptr() as *const c_char
}

/// Short UTT name ("PA", ...).
#[no_mangle]
pub extern "C" fn onair_wisun_utt_short(t: i32) -> *const c_char {
    names::utt_short_z(t).as_ptr() as *const c_char
}

/// Channel function name.
#[no_mangle]
pub extern "C" fn onair_wisun_ch_function_name(f: i32) -> *const c_char {
    names::ch_function_z(f).as_ptr() as *const c_char
}

/// Regulatory domain name.
#[no_mangle]
pub extern "C" fn onair_wisun_reg_domain_name(c: i32) -> *const c_char {
    names::reg_domain_z(c).as_ptr() as *const c_char
}

/// The schedule text; returns the length written, 0 for a schedule that is not present, `usize::MAX` when `cap` is too small.
#[no_mangle]
pub unsafe extern "C" fn onair_wisun_schedule_text(s: *const Schedule, out: *mut u8, cap: usize) -> usize {
    if s.is_null() {
        return 0;
    }
    let mut w = Buf::new(wr(out, cap));
    names::schedule_text(&mut w, &*s, cfn::format_f);
    if w.overflow {
        usize::MAX
    } else {
        w.len()
    }
}
