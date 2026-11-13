// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_uat` (core/include/dect2/rs/uat.h). Every export is `onair_uat_*`; pointers come as (pointer, length) pairs and the caller owns all buffers.
//! A null frame pointer is read as an empty frame (all zero bytes). This is also the only module with `unsafe`: besides the C ABI it holds `math`, the C
//! library `atan2` / `sqrt` the decoder shares with the C reference so the speed and track come out identical.
#![allow(clippy::missing_safety_doc)]

use crate::downlink::{self, Adsb, Math};
use crate::uplink::{self, Uplink};

/// The two C math functions used (the C implementation calls the same ones).
mod math {
    mod c {
        extern "C" {
            pub fn atan2(y: f64, x: f64) -> f64;
            pub fn sqrt(x: f64) -> f64;
        }
    }
    // SAFETY (both): pure C library functions of doubles.
    pub fn atan2(y: f64, x: f64) -> f64 {
        unsafe { c::atan2(y, x) }
    }
    pub fn sqrt(x: f64) -> f64 {
        unsafe { c::sqrt(x) }
    }
}

unsafe fn rd<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(p, n)
    }
}

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_uat_version() -> u32 {
    2
}

/// `sizeof` of the downlink record and the uplink record, for the C++ start-up layout check.
#[no_mangle]
pub unsafe extern "C" fn onair_uat_struct_sizes(out: *mut usize) {
    if !out.is_null() {
        *out = core::mem::size_of::<Adsb>();
        *out.add(1) = core::mem::size_of::<Uplink>();
    }
}

/// Decodes a downlink payload (`n` bytes, at least 31 are read) into `*out`.
#[no_mangle]
pub unsafe extern "C" fn onair_uat_decode_adsb(frame: *const u8, n: usize, out: *mut Adsb) {
    if out.is_null() {
        return;
    }
    let m = Math { atan2: math::atan2, sqrt: math::sqrt };
    *out = downlink::decode_adsb(rd(frame, n), &m);
}

/// Decodes an uplink frame (`n` bytes, 432 are read) into `*out`.
#[no_mangle]
pub unsafe extern "C" fn onair_uat_decode_uplink(frame: *const u8, n: usize, out: *mut Uplink) {
    if out.is_null() {
        return;
    }
    *out = uplink::decode_uplink(rd(frame, n));
}
