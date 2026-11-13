// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_dstar` (core/include/dect2/rs/dstar.h). Every export is `onair_dstar_*`; pointers come as (pointer, length) pairs or point at fixed-size
//! arrays documented in the header; the caller owns all buffers. A null pointer where a buffer is needed makes the function fail (false / -1 / 0), nothing is
//! written. This is also the only module with `unsafe`: besides the C ABI it holds `cfn`, the C library functions (`atof`, `atoi`, `strtoul`, `lround`) that
//! the GPS text parsers share with the C++ reference so numbers come out identical bit for bit.
#![allow(clippy::missing_safety_doc)]

use crate::gps::{self, Fix, Outcome};
use crate::{fec, header};

/// C library number parsers over byte slices (copied to a NUL terminated buffer, like `std::string::c_str()`; text after an embedded NUL is ignored by C).
pub(crate) mod cfn {
    use core::ffi::{c_char, c_int, c_long, c_ulong};
    mod c {
        use core::ffi::{c_char, c_int, c_long, c_ulong};
        extern "C" {
            pub fn atof(s: *const c_char) -> f64;
            pub fn atoi(s: *const c_char) -> c_int;
            pub fn strtoul(s: *const c_char, end: *mut *mut c_char, base: c_int) -> c_ulong;
            pub fn lround(x: f64) -> c_long;
        }
    }
    /// Copies `s` (longer text is cut at 1023 bytes) into a NUL terminated buffer.
    fn z(s: &[u8], buf: &mut [u8; 1024]) {
        let n = s.len().min(buf.len() - 1);
        buf[..n].copy_from_slice(&s[..n]);
        buf[n] = 0;
    }
    /// `atof(s)`.
    pub fn atof(s: &[u8]) -> f64 {
        let mut b = [0u8; 1024];
        z(s, &mut b);
        // SAFETY: `b` is NUL terminated.
        unsafe { c::atof(b.as_ptr() as *const c_char) }
    }
    /// `atoi(s)`.
    pub fn atoi(s: &[u8]) -> i32 {
        let mut b = [0u8; 1024];
        z(s, &mut b);
        // SAFETY: `b` is NUL terminated.
        unsafe { c::atoi(b.as_ptr() as *const c_char) }
    }
    /// `strtoul(s, nullptr, 16)`.
    pub fn strtoul16(s: &[u8]) -> u64 {
        let mut b = [0u8; 1024];
        z(s, &mut b);
        // SAFETY: `b` is NUL terminated; a null end pointer is allowed.
        unsafe { c::strtoul(b.as_ptr() as *const c_char, core::ptr::null_mut(), 16 as c_int) as c_ulong as u64 }
    }
    /// `lround(x)` (a C `long`, widened).
    pub fn lround(x: f64) -> i64 {
        // SAFETY: a pure C library function of one double.
        let r: c_long = unsafe { c::lround(x) };
        r as i64
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
/// A fixed-size array behind a pointer.
unsafe fn ar<'a, T, const N: usize>(p: *const T) -> Option<&'a [T; N]> {
    if p.is_null() {
        None
    } else {
        Some(&*(p as *const [T; N]))
    }
}
/// A fixed-size mutable array behind a pointer.
unsafe fn am<'a, T, const N: usize>(p: *mut T) -> Option<&'a mut [T; N]> {
    if p.is_null() {
        None
    } else {
        Some(&mut *(p as *mut [T; N]))
    }
}

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_dstar_version() -> u32 {
    2
}

/// A position fix.
#[repr(C)]
pub struct OnairDstarFix {
    pub lat: f64,
    pub lon: f64,
    pub alt_m: f64,
    pub speed_kmh: f64,
    pub course: i32,
    pub alt_valid: u8,
    pub vel_valid: u8,
    pub aprs: u8,
}
/// An unpacked header: NUL terminated callsign fields.
#[repr(C)]
pub struct OnairDstarHeader {
    pub flag: [u8; 3],
    pub rpt2: [u8; 9],
    pub rpt1: [u8; 9],
    pub ur: [u8; 9],
    pub my: [u8; 9],
    pub sfx: [u8; 9],
}

/// sizeof of the two shared structs (fix, header): the C++ side checks them at start-up.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_struct_sizes(out: *mut usize) {
    if let Some(o) = am::<usize, 2>(out) {
        *o = [core::mem::size_of::<OnairDstarFix>(), core::mem::size_of::<OnairDstarHeader>()];
    }
}

/// CRC-CCITT as D-STAR uses it over `n` bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_crc(p: *const u8, n: usize) -> u16 {
    header::crc(rd(p, n))
}

/// Packs a header (3 flag bytes and five fields given as pointer + length, cut / space padded) into 41 bytes with its CRC.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_header_pack(
    flag: *const u8,
    rpt2: *const u8,
    rpt2_n: usize,
    rpt1: *const u8,
    rpt1_n: usize,
    ur: *const u8,
    ur_n: usize,
    my: *const u8,
    my_n: usize,
    sfx: *const u8,
    sfx_n: usize,
    out41: *mut u8,
) {
    if let (Some(f), Some(o)) = (ar::<u8, 3>(flag), am::<u8, 41>(out41)) {
        header::pack(*f, rd(rpt2, rpt2_n), rd(rpt1, rpt1_n), rd(ur, ur_n), rd(my, my_n), rd(sfx, sfx_n), o);
    }
}

fn text_out(t: &header::Text, o: &mut [u8; 9]) {
    *o = [0; 9];
    o[..t.n].copy_from_slice(&t.b[..t.n]);
}
/// Unpacks 41 bytes into `*h` (always written); returns false when the CRC does not match.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_header_unpack(inp: *const u8, h: *mut OnairDstarHeader) -> bool {
    match (ar::<u8, 41>(inp), h.is_null()) {
        (Some(i), false) => {
            let (r, ok) = header::unpack(i);
            let h = &mut *h;
            h.flag = r.flag;
            text_out(&r.rpt2, &mut h.rpt2);
            text_out(&r.rpt1, &mut h.rpt1);
            text_out(&r.ur, &mut h.ur);
            text_out(&r.my, &mut h.my);
            text_out(&r.sfx, &mut h.sfx);
            ok
        }
        _ => false,
    }
}
/// Text of the flag bytes into `out` (at most 96 bytes are written); returns its length.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_flag_text(f: *const u8, out: *mut u8, cap: usize) -> usize {
    match ar::<u8, 3>(f) {
        Some(f) => header::flag_text(f, wr(out, cap)),
        None => 0,
    }
}

/// Header on the air: 41 bytes -> 660 bits.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_header_encode(hdr: *const u8, bits: *mut u8) {
    if let (Some(h), Some(b)) = (ar::<u8, 41>(hdr), am::<u8, 660>(bits)) {
        fec::header_encode(h, b);
    }
}
/// Header decode of 660 soft values (positive = 1) into 41 bytes; returns the channel bit errors corrected, or -1 on a null pointer.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_header_decode(soft: *const f32, hdr: *mut u8) -> i32 {
    match (ar::<f32, 660>(soft), am::<u8, 41>(hdr)) {
        (Some(s), Some(h)) => fec::header_decode(s, h),
        _ => -1,
    }
}
/// XORs the scrambler sequence into `n` bits.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_scramble_bits(bits: *mut u8, n: usize) {
    fec::scramble_bits(wr(bits, n));
}
/// Interleaves 660 bits.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_interleave(inp: *const u8, out: *mut u8) {
    if let (Some(i), Some(o)) = (ar::<u8, 660>(inp), am::<u8, 660>(out)) {
        fec::interleave(i, o);
    }
}
/// De-interleaves 660 soft values.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_deinterleave(inp: *const f32, out: *mut f32) {
    if let (Some(i), Some(o)) = (ar::<f32, 660>(inp), am::<f32, 660>(out)) {
        fec::deinterleave(i, o);
    }
}
/// Convolutional encode `n_in` bits into `2 * n_in` bits.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_conv_encode(inp: *const u8, n_in: usize, out: *mut u8) {
    fec::conv_encode(rd(inp, n_in), wr(out, 2 * n_in));
}
/// Soft Viterbi: `n_out` bits from `2 * n_out` soft values; returns the corrected channel bits, or -1 when `n_out` is above 1024 or a pointer is null.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_viterbi(soft: *const f32, n_out: usize, out: *mut u8) -> i32 {
    if soft.is_null() || out.is_null() || n_out > fec::MAX_VITERBI {
        return -1;
    }
    let s = core::slice::from_raw_parts(soft, 2 * n_out);
    fec::viterbi(s, n_out, wr(out, n_out)).unwrap_or(-1)
}
/// Bytes to bits, LSB first.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_bytes_to_bits(b: *const u8, n_bytes: usize, bits: *mut u8) {
    fec::bytes_to_bits(rd(b, n_bytes), wr(bits, 8 * n_bytes));
}
/// Bits to bytes, LSB first.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_bits_to_bytes(bits: *const u8, n_bytes: usize, b: *mut u8) {
    fec::bits_to_bytes(rd(bits, 8 * n_bytes), wr(b, n_bytes));
}
/// 72 voice bits into mbelib's `ambe_fr[4][24]` (96 bytes).
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_ambe_frame(bits: *const u8, fr: *mut u8) {
    if let (Some(b), Some(f)) = (ar::<u8, 72>(bits), am::<u8, 96>(fr)) {
        header::ambe_frame(b, f);
    }
}

fn fix_to_c(f: &Fix) -> OnairDstarFix {
    OnairDstarFix {
        lat: f.lat,
        lon: f.lon,
        alt_m: f.alt_m,
        speed_kmh: f.speed_kmh,
        course: f.course,
        alt_valid: f.alt_valid as u8,
        vel_valid: f.vel_valid as u8,
        aprs: f.aprs as u8,
    }
}
fn code(o: Outcome) -> i32 {
    match o {
        Outcome::Untouched => 0,
        Outcome::Partial => 1,
        Outcome::Parsed => 2,
    }
}
/// The two hex digits of the NMEA checksum of `body` (n bytes) into `out[2]`.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_nmea_checksum(body: *const u8, n: usize, out: *mut u8) {
    if let Some(o) = am::<u8, 2>(out) {
        *o = gps::nmea_checksum(rd(body, n));
    }
}
/// CRC of a D-PRS text (after "$$CRCxxxx,").
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_dprs_crc(s: *const u8, n: usize) -> u16 {
    gps::dprs_crc(rd(s, n))
}
/// Parses an NMEA line. 0: rejected, `*f` is left as it was; 1: rejected after the reference had written to the fix (`*f` holds that state); 2: parsed.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_parse_nmea(line: *const u8, n: usize, f: *mut OnairDstarFix) -> i32 {
    if f.is_null() {
        return 0;
    }
    let mut fix = Fix::default();
    let o = gps::parse_nmea(rd(line, n), &mut fix);
    if o != Outcome::Untouched {
        *f = fix_to_c(&fix);
    }
    code(o)
}
/// Parses a D-PRS line; `call` and `comment` need `n` bytes each (their lengths are returned in `*call_len` and `*comment_len`). Result codes as for NMEA.
#[no_mangle]
pub unsafe extern "C" fn onair_dstar_parse_dprs(
    line: *const u8,
    n: usize,
    f: *mut OnairDstarFix,
    call: *mut u8,
    call_len: *mut usize,
    comment: *mut u8,
    comment_len: *mut usize,
) -> i32 {
    if f.is_null() || call.is_null() || comment.is_null() {
        return 0;
    }
    let mut fix = Fix::default();
    let (o, t) = gps::parse_dprs(rd(line, n), &mut fix, wr(call, n), wr(comment, n));
    if o != Outcome::Untouched {
        *f = fix_to_c(&fix);
    }
    if !call_len.is_null() {
        *call_len = t.call_len;
    }
    if !comment_len.is_null() {
        *comment_len = t.comment_len;
    }
    code(o)
}
