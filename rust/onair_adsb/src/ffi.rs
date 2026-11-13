// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_adsb` (core/include/dect2/rs/adsb.h). Every export is `onair_adsb_*`; pointers come as (pointer, length) pairs and the caller owns
//! all buffers. Functions that write text return the length written, or `usize::MAX` when the buffer is too small. Out parameters are written only
//! when the function returns true (like the C++ reference).
//!
//! This is also the only module with `unsafe`: besides the C ABI it holds `math`, the C library functions that `no_std` Rust has no methods for.
#![allow(clippy::missing_safety_doc)]

use crate::commb::{self, CommB};
use crate::msg::{self, Msg};
use crate::text::Buf;
use crate::{altitude, cpr, crc, geo};

/// The C math library (floor, fmod, sqrt, trig, exp, pow...). The C++ implementation calls the same functions, so the results are identical; they
/// are linked by every C++ program anyway and by `std` in the test builds.
pub(crate) mod math {
    mod c {
        extern "C" {
            pub fn floor(x: f64) -> f64;
            pub fn fmod(x: f64, y: f64) -> f64;
            pub fn fabs(x: f64) -> f64;
            pub fn sqrt(x: f64) -> f64;
            pub fn hypot(x: f64, y: f64) -> f64;
            pub fn sin(x: f64) -> f64;
            pub fn cos(x: f64) -> f64;
            pub fn tan(x: f64) -> f64;
            pub fn asin(x: f64) -> f64;
            pub fn atan2(y: f64, x: f64) -> f64;
            pub fn exp(x: f64) -> f64;
            pub fn pow(x: f64, y: f64) -> f64;
        }
    }
    macro_rules! wrap1 {
        ($($n:ident),*) => { $(
            // SAFETY: a pure C library function of one double.
            pub fn $n(x: f64) -> f64 { unsafe { c::$n(x) } }
        )* };
    }
    macro_rules! wrap2 {
        ($($n:ident),*) => { $(
            // SAFETY: a pure C library function of two doubles.
            pub fn $n(x: f64, y: f64) -> f64 { unsafe { c::$n(x, y) } }
        )* };
    }
    wrap1!(floor, fabs, sqrt, sin, cos, tan, asin, exp);
    wrap2!(fmod, hypot, atan2, pow);
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
/// The `nbits / 8` bytes of a message.
unsafe fn msg_bytes<'a>(p: *const u8, nbits: i32) -> &'a [u8] {
    rd(p, if nbits > 0 { (nbits / 8) as usize } else { 0 })
}
unsafe fn put<T>(p: *mut T, v: T) {
    if !p.is_null() {
        *p = v;
    }
}

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_adsb_version() -> u32 {
    1
}

/// sizeof(OnairAdsbMsg) and sizeof(OnairAdsbCommB): the C++ side checks them against its own structs when the program starts.
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_struct_sizes(msg: *mut usize, commb: *mut usize) {
    put(msg, core::mem::size_of::<Msg>());
    put(commb, core::mem::size_of::<CommB>());
}

#[no_mangle]
pub unsafe extern "C" fn onair_adsb_crc24(m: *const u8, nbits: i32) -> u32 {
    crc::crc24(msg_bytes(m, nbits), nbits)
}
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_crc_parity(m: *const u8, nbits: i32) -> u32 {
    crc::parity(msg_bytes(m, nbits), nbits)
}
#[no_mangle]
pub extern "C" fn onair_adsb_df_length(df: i32) -> i32 {
    crc::df_length(df)
}
#[no_mangle]
pub extern "C" fn onair_adsb_single_bit_position(nbits: i32, rem: u32) -> i32 {
    crc::single_bit_position(nbits, rem)
}
#[no_mangle]
pub extern "C" fn onair_adsb_single_bit_syndrome(nbits: i32, pos: i32) -> u32 {
    crc::single_bit_syndrome(nbits, pos)
}

#[no_mangle]
pub unsafe extern "C" fn onair_adsb_decode_ac13(ac: u32, alt: *mut i32) -> bool {
    altitude::decode_ac13(ac).map(|a| put(alt, a)).is_some()
}
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_decode_alt12(a12: u32, alt: *mut i32) -> bool {
    altitude::decode_alt12(a12).map(|a| put(alt, a)).is_some()
}
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_gillham_to_altitude(code: u32, alt: *mut i32) -> bool {
    altitude::gillham_to_altitude(code).map(|a| put(alt, a)).is_some()
}
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_altitude_to_gillham(alt: i32, code: *mut u32) -> bool {
    altitude::altitude_to_gillham(alt).map(|c| put(code, c)).is_some()
}
#[no_mangle]
pub extern "C" fn onair_adsb_decode_id13(id: u32) -> i32 {
    altitude::decode_id13(id)
}
#[no_mangle]
pub extern "C" fn onair_adsb_encode_id13(sq: i32) -> u32 {
    altitude::encode_id13(sq)
}
#[no_mangle]
pub extern "C" fn onair_adsb_char_from_6bit(c: u32) -> u8 {
    altitude::char_from_6bit(c)
}
#[no_mangle]
pub extern "C" fn onair_adsb_char_to_6bit(ch: u32) -> i32 {
    let ch = ch as u8;
    altitude::char_to_6bit(ch)
}
/// Category code ("A3") into `out` (2 bytes); the length written, 0 outside tc 1 - 4 / ca 0 - 7.
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_category_code(tc: i32, ca: i32, out: *mut u8) -> usize {
    match altitude::category_code(tc, ca) {
        Some(c) if !out.is_null() => {
            *out = c[0];
            *out.add(1) = c[1];
            2
        }
        Some(_) => 2,
        None => 0,
    }
}
/// Static NUL-terminated strings ("" when none).
#[no_mangle]
pub extern "C" fn onair_adsb_category_name(tc: i32, ca: i32) -> *const u8 {
    altitude::category_name(tc, ca).as_ptr()
}
#[no_mangle]
pub extern "C" fn onair_adsb_emergency_name(s: i32) -> *const u8 {
    altitude::emergency_name(s).as_ptr()
}

#[no_mangle]
pub extern "C" fn onair_adsb_cpr_nl(lat: f64) -> i32 {
    cpr::nl(lat)
}
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_cpr_global_airborne(
    le: i32,
    xe: i32,
    lo: i32,
    xo: i32,
    odd_latest: u32,
    lat: *mut f64,
    lon: *mut f64,
) -> bool {
    let odd_latest = odd_latest != 0;
    cpr::global_airborne(le, xe, lo, xo, odd_latest).map(|p| (put(lat, p.lat), put(lon, p.lon))).is_some()
}
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_cpr_local_airborne(
    lc: i32,
    xc: i32,
    odd: u32,
    ref_lat: f64,
    ref_lon: f64,
    lat: *mut f64,
    lon: *mut f64,
) -> bool {
    let odd = odd != 0;
    cpr::local_airborne(lc, xc, odd, ref_lat, ref_lon).map(|p| (put(lat, p.lat), put(lon, p.lon))).is_some()
}
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn onair_adsb_cpr_global_surface(
    le: i32,
    xe: i32,
    lo: i32,
    xo: i32,
    odd_latest: u32,
    ref_lat: f64,
    ref_lon: f64,
    lat: *mut f64,
    lon: *mut f64,
) -> bool {
    let odd_latest = odd_latest != 0;
    cpr::global_surface(le, xe, lo, xo, odd_latest, ref_lat, ref_lon).map(|p| (put(lat, p.lat), put(lon, p.lon))).is_some()
}
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_cpr_local_surface(
    lc: i32,
    xc: i32,
    odd: u32,
    ref_lat: f64,
    ref_lon: f64,
    lat: *mut f64,
    lon: *mut f64,
) -> bool {
    let odd = odd != 0;
    cpr::local_surface(lc, xc, odd, ref_lat, ref_lon).map(|p| (put(lat, p.lat), put(lon, p.lon))).is_some()
}
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_cpr_encode_airborne(lat: f64, lon: f64, odd: u32, lat_cpr: *mut i32, lon_cpr: *mut i32) {
    let odd = odd != 0;
    let (a, b) = cpr::encode_airborne(lat, lon, odd);
    put(lat_cpr, a);
    put(lon_cpr, b);
}
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_cpr_encode_surface(lat: f64, lon: f64, odd: u32, lat_cpr: *mut i32, lon_cpr: *mut i32) {
    let odd = odd != 0;
    let (a, b) = cpr::encode_surface(lat, lon, odd);
    put(lat_cpr, a);
    put(lon_cpr, b);
}

#[no_mangle]
pub extern "C" fn onair_adsb_distance_nm(lat1: f64, lon1: f64, lat2: f64, lon2: f64) -> f64 {
    geo::distance_nm(lat1, lon1, lat2, lon2)
}
#[no_mangle]
pub extern "C" fn onair_adsb_bearing_deg(lat1: f64, lon1: f64, lat2: f64, lon2: f64) -> f64 {
    geo::bearing_deg(lat1, lon1, lat2, lon2)
}
#[no_mangle]
pub extern "C" fn onair_adsb_mag_declination_deg(lat: f64, lon: f64) -> f64 {
    geo::mag_declination_deg(lat, lon)
}
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_wind_triangle(trk: f64, gs: f64, hdg: f64, tas: f64, from_deg: *mut f64, kt: *mut f64) {
    let (f, k) = geo::wind_triangle(trk, gs, hdg, tas);
    put(from_deg, f);
    put(kt, k);
}
#[no_mangle]
pub extern "C" fn onair_adsb_sat_from_tas_mach(tas: f64, mach: f64) -> f64 {
    geo::sat_from_tas_mach(tas, mach)
}
#[no_mangle]
pub extern "C" fn onair_adsb_isa_pressure_hpa(alt_ft: f64) -> f64 {
    geo::isa_pressure_hpa(alt_ft)
}
#[no_mangle]
pub extern "C" fn onair_adsb_isa_temp_c(alt_ft: f64) -> f64 {
    geo::isa_temp_c(alt_ft)
}

/// Comm-B inference on the 7 bytes of the MB field.
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_infer_commb(mb: *const u8, alt_ft: i32, out: *mut CommB) {
    put(out, commb::infer(rd(mb, 7), alt_ft));
}
/// The text of an ACAS resolution advisory.
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_ra_text(ara: u32, rac: u32, terminated: u32, out: *mut u8, cap: usize) -> usize {
    let terminated = terminated != 0;
    let mut b = Buf::new(wr(out, cap));
    commb::ra_text(ara, rac, terminated, &mut b);
    if b.overflow {
        usize::MAX
    } else {
        b.len()
    }
}

/// Field extraction of an `nbits` message (the caller has done CRC and address checks); the message is written even when false is returned.
#[no_mangle]
pub unsafe extern "C" fn onair_adsb_decode_msg(bytes: *const u8, nbits: i32, out: *mut Msg) -> bool {
    let (ok, m) = msg::decode(msg_bytes(bytes, nbits), nbits);
    put(out, m);
    ok
}
