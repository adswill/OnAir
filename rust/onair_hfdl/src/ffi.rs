// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_hfdl` (core/include/dect2/rs/hfdl.h). Every export is `onair_hfdl_*`; pointers come as (pointer, length) pairs and the caller owns all buffers.
//! A null pointer with a length is read as an empty input.
//!
//! This is also the only module with `unsafe`: besides the C ABI it holds `cfn`, the C library `snprintf("%.Nf")` that formats numbers in the detail text, so the
//! digits equal the C++ reference.
#![allow(clippy::missing_safety_doc)]

use crate::parse::{self, Msg};
use crate::phy::{self, Rate};
use crate::stations;
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
    /// Writes `v` as C `%.{prec}f` (prec 0 .. 4) into `out` (64 bytes); returns the length (at most 63).
    pub fn format_f(prec: u8, v: f64, out: &mut [u8; 64]) -> usize {
        const FMT: [&[u8]; 5] = [b"%.0f\0", b"%.1f\0", b"%.2f\0", b"%.3f\0", b"%.4f\0"];
        let f = FMT[usize::from(prec.min(4))];
        // SAFETY: `out` is 64 writable bytes, `f` is NUL terminated with one double conversion, one double is passed.
        let n: c_int = unsafe { c::snprintf(out.as_mut_ptr() as *mut c_char, out.len(), f.as_ptr() as *const c_char, v) };
        if n < 0 {
            0
        } else {
            (n as usize).min(out.len() - 1)
        }
    }
}

unsafe fn rd<'a, T>(p: *const T, n: usize) -> &'a [T] {
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
pub extern "C" fn onair_hfdl_version() -> u32 {
    2
}

/// `sizeof` of Rate, Msg and StationOut in this order (`out` has room for 3): the C++ side checks them at start-up.
#[no_mangle]
pub unsafe extern "C" fn onair_hfdl_struct_sizes(out: *mut usize) {
    if !out.is_null() {
        *out = core::mem::size_of::<Rate>();
        *out.add(1) = core::mem::size_of::<Msg>();
        *out.add(2) = core::mem::size_of::<StationOut>();
    }
}

/// The rate for the M1 shift index (clamped into 0 .. 7).
#[no_mangle]
pub unsafe extern "C" fn onair_hfdl_rate(m1: i32, out: *mut Rate) {
    if !out.is_null() {
        *out = *phy::rate(m1);
    }
}
/// Symbols from A1 to the last probe (the prekey not counted).
#[no_mangle]
pub extern "C" fn onair_hfdl_burst_syms(m1: i32) -> i32 {
    phy::burst_syms(m1)
}
/// Bits on the channel.
#[no_mangle]
pub extern "C" fn onair_hfdl_coded_bits(m1: i32) -> i32 {
    phy::coded_bits(m1)
}
/// Octets one burst carries.
#[no_mangle]
pub extern "C" fn onair_hfdl_payload_octets(m1: i32) -> i32 {
    phy::payload_octets(m1)
}
/// Bit `k` (0 .. 126) of the A sequence.
#[no_mangle]
pub extern "C" fn onair_hfdl_a_bit(k: i32) -> i32 {
    phy::a_bit(k)
}
/// Bit `k` (0 .. 126) of the M sequence with cyclic shift `shift` (0 .. 7).
#[no_mangle]
pub extern "C" fn onair_hfdl_m_bit(shift: i32, k: i32) -> i32 {
    phy::m_bit(shift, k)
}
/// The 531 known symbols (+-1) from A1 to the last training block for rate `m1` into `out` (capacity `cap`, 531 needed); false when too small.
#[no_mangle]
pub unsafe extern "C" fn onair_hfdl_preamble_ref(m1: i32, out: *mut i8, cap: usize) -> bool {
    phy::preamble_ref(m1, wr(out, cap))
}
/// Scrambler signs (+1 / -1) for `n` data symbols.
#[no_mangle]
pub unsafe extern "C" fn onair_hfdl_scrambler_signs(out: *mut i8, n: usize) {
    phy::scrambler_signs(wr(out, n));
}
/// The interleaver of rate `m1` into `out` (capacity `cap` ints, twice the coded bits needed: the second half is scratch). Returns the coded bit count (the
/// permutation is in `out[..count]`) or `SIZE_MAX` when `cap` is too small.
#[no_mangle]
pub unsafe extern "C" fn onair_hfdl_interleave(m1: i32, out: *mut i32, cap: usize) -> usize {
    phy::interleave(m1, wr(out, cap))
}
/// Convolutional encoder: `n` bits (one per byte) into `2 * n` coded bits; false when `cap` is smaller.
#[no_mangle]
pub unsafe extern "C" fn onair_hfdl_conv_encode(bits: *const u8, n: usize, out: *mut u8, cap: usize) -> bool {
    phy::conv_encode(rd(bits, n), wr(out, cap))
}
/// Soft Viterbi decoder: `soft` holds `2 * n` values (positive = 0), `n` decoded bits go to `out`; `surv` is scratch of `n` u64. False when a buffer is too small.
#[no_mangle]
pub unsafe extern "C" fn onair_hfdl_viterbi(
    soft: *const f32,
    soft_len: usize,
    n: usize,
    out: *mut u8,
    out_cap: usize,
    surv: *mut u64,
    surv_cap: usize,
) -> bool {
    phy::viterbi(rd(soft, soft_len), n, wr(out, out_cap), wr(surv, surv_cap))
}
/// The FCS of `n` octets.
#[no_mangle]
pub unsafe extern "C" fn onair_hfdl_fcs(p: *const u8, n: usize) -> u16 {
    phy::fcs(rd(p, n))
}
/// True when the `n` octets are followed by their FCS (`n + 2` octets are read).
#[no_mangle]
pub unsafe extern "C" fn onair_hfdl_fcs_ok(p: *const u8, n: usize) -> bool {
    match n.checked_add(2) {
        Some(m) => phy::fcs_ok(rd(p, m)),
        None => false,
    }
}

/// One ground station for the C side (`name` is static and NUL terminated).
#[repr(C)]
pub struct StationOut {
    pub id: i32,
    pub nkhz: i32,
    pub lat: f64,
    pub lon: f64,
    pub khz: [i32; 8],
    pub name: *const c_char,
}

/// Number of entries in the station table.
#[no_mangle]
pub extern "C" fn onair_hfdl_station_count() -> usize {
    stations::STATIONS.len()
}
/// Entry `i` of the station table; false when out of range.
#[no_mangle]
pub unsafe extern "C" fn onair_hfdl_station_get(i: usize, out: *mut StationOut) -> bool {
    if out.is_null() || i >= stations::STATIONS.len() {
        return false;
    }
    let s = &stations::STATIONS[i];
    let mut khz = [0i32; 8];
    let n = s.khz.len().min(8);
    khz[..n].copy_from_slice(&s.khz[..n]);
    *out = StationOut { id: s.id, nkhz: n as i32, lat: s.lat, lon: s.lon, khz, name: s.name.as_ptr() as *const c_char };
    true
}
/// The frequencies a 20-bit mask selects at station `gs_id` into `out[20]`; returns the count (a bit beyond the table gives `-1 - bit`).
#[no_mangle]
pub unsafe extern "C" fn onair_hfdl_mask_khz(gs_id: i32, mask: u32, out: *mut i32) -> usize {
    if out.is_null() {
        return 0;
    }
    stations::mask_khz(gs_id, mask, &mut *(out as *mut [i32; 20]))
}
/// The first station that lists this frequency (0 none).
#[no_mangle]
pub extern "C" fn onair_hfdl_station_on_khz(khz: i32) -> i32 {
    stations::station_on_khz(khz)
}

/// Parses the PDU of one burst into up to `cap` messages (128 cover any PDU); `*count` receives how many. False when no check sequence comes out right.
#[no_mangle]
pub unsafe extern "C" fn onair_hfdl_parse_pdu(p: *const u8, n: usize, out: *mut Msg, cap: usize, count: *mut usize) -> bool {
    match parse::parse_pdu(rd(p, n), wr(out, cap)) {
        Some(c) => {
            if !count.is_null() {
                *count = c;
            }
            true
        }
        None => false,
    }
}
