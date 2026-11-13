// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_vdl2` (core/include/dect2/rs/vdl2.h). Every export is `onair_vdl2_*`; pointers come as (pointer, length) pairs and the caller owns all buffers.
//! A null pointer with a length is read as an empty input. Fixed-size arrays (parity, codewords, addresses) must be present.
//!
//! This is also the only module with `unsafe`: besides the C ABI it holds `cfn`, the C library `snprintf("%.1f")` that formats numbers in the detail text, so the
//! digits equal the C++ reference.
#![allow(clippy::missing_safety_doc)]

use crate::avlc::{self, Addr};
use crate::bits::{self, Layout, RS_K, RS_N};
use crate::proto::Parsed;
use crate::{burst, rs};
use core::ffi::c_char;

/// C library `snprintf("%.Nf")` for the digits of numbers in text.
pub(crate) mod cfn {
    use core::ffi::{c_char, c_int};
    mod c {
        use core::ffi::{c_char, c_int};
        extern "C" {
            pub fn snprintf(s: *mut c_char, n: usize, fmt: *const c_char, ...) -> c_int;
        }
    }
    /// Writes `v` as C `%.{prec}f` (prec 0 or 1) into `out` (64 bytes); returns the length (at most 63).
    pub fn format_f(prec: u8, v: f64, out: &mut [u8; 64]) -> usize {
        const FMT: [&[u8]; 2] = [b"%.0f\0", b"%.1f\0"];
        let f = FMT[usize::from(prec.min(1))];
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
pub extern "C" fn onair_vdl2_version() -> u32 {
    2
}

/// `sizeof` of Parsed, Layout and Addr in this order (`out` has room for 3): the C++ side checks them at start-up.
#[no_mangle]
pub unsafe extern "C" fn onair_vdl2_struct_sizes(out: *mut usize) {
    if !out.is_null() {
        *out = core::mem::size_of::<Parsed>();
        *out.add(1) = core::mem::size_of::<Layout>();
        *out.add(2) = core::mem::size_of::<Addr>();
    }
}

/// Scrambles (or descrambles) `n` bits, one per byte, in place.
#[no_mangle]
pub unsafe extern "C" fn onair_vdl2_scramble(bits: *mut u8, n: usize) {
    bits::scramble(wr(bits, n));
}

/// The 25 header bits for a burst of `len_bits` data bits.
#[no_mangle]
pub extern "C" fn onair_vdl2_header_word(len_bits: u32) -> u32 {
    bits::header_word(len_bits)
}

/// Corrects and reads a header word: `*corrected` is always written, `*len` when the corrected word is in range; returns true for a plausible header.
#[no_mangle]
pub unsafe extern "C" fn onair_vdl2_header_decode(word: u32, len: *mut u32, corrected: *mut bool) -> bool {
    let h = bits::header_decode(word);
    if !corrected.is_null() {
        *corrected = h.corrected;
    }
    if h.have_len && !len.is_null() {
        *len = h.len;
    }
    h.ok
}

/// The parity octets of the last block of `n` data octets.
#[no_mangle]
pub extern "C" fn onair_vdl2_last_fec(n: i32) -> i32 {
    bits::last_fec(n)
}

/// The layout for `len_bits` data bits (always written); false when it cannot be a burst.
#[no_mangle]
pub unsafe extern "C" fn onair_vdl2_layout(len_bits: u32, out: *mut Layout) -> bool {
    let (l, ok) = bits::layout(len_bits);
    if !out.is_null() {
        *out = l;
    }
    ok
}

/// Reed-Solomon parity of 249 data octets (6 octets out).
#[no_mangle]
pub unsafe extern "C" fn onair_vdl2_rs_encode(data: *const u8, parity: *mut u8) {
    if data.is_null() || parity.is_null() {
        return;
    }
    let mut d = [0u8; RS_K];
    d.copy_from_slice(core::slice::from_raw_parts(data, RS_K));
    let p = rs::encode(&d);
    core::ptr::copy_nonoverlapping(p.as_ptr(), parity, p.len());
}

/// Corrects a 255 octet codeword in place with `n_parity` parity octets present; returns the octets corrected or -1.
#[no_mangle]
pub unsafe extern "C" fn onair_vdl2_rs_decode(cw: *mut u8, n_parity: i32) -> i32 {
    if cw.is_null() {
        return -1;
    }
    let mut c = [0u8; RS_N];
    c.copy_from_slice(core::slice::from_raw_parts(cw, RS_N));
    let r = rs::decode(&mut c, n_parity);
    core::ptr::copy_nonoverlapping(c.as_ptr(), cw, RS_N);
    r
}

/// Gathers and corrects a burst body: `bits[0..n]` (one bit per byte) with layout `l`, the first `len_bits` data bits go to `out` (capacity `cap`). Returns the
/// bits written or -1 when a block cannot be corrected; `*rs_fixed` receives the octets corrected (also on failure).
#[no_mangle]
pub unsafe extern "C" fn onair_vdl2_burst_data(
    bits: *const u8,
    n: usize,
    l: *const Layout,
    len_bits: u32,
    out: *mut u8,
    cap: usize,
    rs_fixed: *mut i32,
) -> i64 {
    if l.is_null() {
        return -1;
    }
    let (r, fixed) = burst::burst_data(rd(bits, n), &*l, len_bits, wr(out, cap));
    if !rs_fixed.is_null() {
        *rs_fixed = fixed;
    }
    match r {
        Some(c) => c as i64,
        None => -1,
    }
}

/// The FCS to send for `n` frame octets.
#[no_mangle]
pub unsafe extern "C" fn onair_vdl2_fcs(p: *const u8, n: usize) -> u16 {
    burst::fcs(rd(p, n))
}

/// True when the `n` octets, FCS included, leave the CRC at its residue.
#[no_mangle]
pub unsafe extern "C" fn onair_vdl2_fcs_ok(p: *const u8, n: usize) -> bool {
    burst::fcs_ok(rd(p, n))
}

/// Extracts the HDLC frames from `n` bits (one per byte): the octets go one after the other into `out` (capacity `out_cap`), the frame lengths into `lens`
/// (capacity `lens_cap`). `*frames` and `*octets` receive the counts. False when a buffer is too small (`n / 8 + 1` octets and `n / 16 + 1` frames suffice).
#[no_mangle]
pub unsafe extern "C" fn onair_vdl2_hdlc_frames(
    bits: *const u8,
    n: usize,
    out: *mut u8,
    out_cap: usize,
    lens: *mut u32,
    lens_cap: usize,
    frames: *mut usize,
    octets: *mut usize,
) -> bool {
    let l: &mut [u32] = if lens.is_null() || lens_cap == 0 { &mut [] } else { core::slice::from_raw_parts_mut(lens, lens_cap) };
    match burst::hdlc_frames(rd(bits, n), wr(out, out_cap), l) {
        Some((f, o)) => {
            if !frames.is_null() {
                *frames = f;
            }
            if !octets.is_null() {
                *octets = o;
            }
            true
        }
        None => false,
    }
}

/// Writes an AVLC address into 4 octets.
#[no_mangle]
pub unsafe extern "C" fn onair_vdl2_put_addr(a: *const Addr, last: u32, out4: *mut u8) {
    let last = last != 0;
    if a.is_null() || out4.is_null() {
        return;
    }
    let o = avlc::put_addr(&*a, last);
    core::ptr::copy_nonoverlapping(o.as_ptr(), out4, 4);
}

/// Reads an AVLC address from 4 octets.
#[no_mangle]
pub unsafe extern "C" fn onair_vdl2_get_addr(in4: *const u8, out: *mut Addr) {
    if in4.is_null() || out.is_null() {
        return;
    }
    let s = core::slice::from_raw_parts(in4, 4);
    *out = avlc::get_addr(&[s[0], s[1], s[2], s[3]]);
}

/// Address type name (static NUL terminated text).
#[no_mangle]
pub extern "C" fn onair_vdl2_addr_type_name(t: i32) -> *const c_char {
    avlc::addr_type_name_z(t).as_ptr() as *const c_char
}

/// Decodes an AVLC frame of `n` octets (FCS removed) into `*out`; false when it is too short.
#[no_mangle]
pub unsafe extern "C" fn onair_vdl2_parse_frame(f: *const u8, n: usize, out: *mut Parsed) -> bool {
    if out.is_null() {
        return false;
    }
    match avlc::parse_frame(rd(f, n)) {
        Some(p) => {
            *out = p;
            true
        }
        None => false,
    }
}
