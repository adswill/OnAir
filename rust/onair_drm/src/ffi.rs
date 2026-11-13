// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_drm` (core/include/dect2/rs/drm.h). Every export is `onair_drm_*`; pointers come as (pointer, length) pairs and the caller owns all buffers.
//! Bits are one per byte (0 or 1), most significant first. Names are written into a caller buffer and the length returned. A null pointer where a buffer is
//! needed makes a function fail (false / 0), nothing is written. `onair_drm_struct_sizes` lets the C++ side check the record layouts at start-up.
#![allow(clippy::missing_safety_doc)]

use crate::aac::{self, AacInfo};
use crate::fac::{self, FacInfo};
use crate::sdc::{self, SdcInfo};
use crate::text::{self, TextSeg};
use crate::{bits, data, names};

/// Input slice (empty for a null pointer or length 0).
unsafe fn rd<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(p, n)
    }
}

/// Output slice (empty for a null pointer or capacity 0).
unsafe fn wr<'a>(p: *mut u8, n: usize) -> &'a mut [u8] {
    if p.is_null() || n == 0 {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(p, n)
    }
}

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_drm_version() -> u32 {
    1
}

/// sizeof of the record structs in the order FacInfo, SdcInfo, AacInfo, TextSeg.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_struct_sizes(out: *mut usize) {
    if out.is_null() {
        return;
    }
    let v = [
        core::mem::size_of::<FacInfo>(),
        core::mem::size_of::<SdcInfo>(),
        core::mem::size_of::<AacInfo>(),
        core::mem::size_of::<TextSeg>(),
    ];
    for (i, x) in v.iter().enumerate() {
        *out.add(i) = *x;
    }
}

/// CRC-8 of the FAC over `n` bits.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_crc8_bits(b: *const u8, n: usize) -> u32 {
    bits::crc8(rd(b, n))
}

/// CRC-16 of the SDC over `n` bits.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_crc16_bits(b: *const u8, n: usize) -> u32 {
    bits::crc16(rd(b, n))
}

/// Parse a descrambled FAC block (72 or 116 bits); false when the CRC is wrong or the RM flag disagrees.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_fac_parse(b: *const u8, n: usize, mode_e: u32, out: *mut FacInfo) -> bool {
    let mode_e = mode_e != 0;
    match fac::fac_parse(rd(b, n), mode_e) {
        Some(f) if !out.is_null() => {
            *out = f;
            true
        }
        _ => false,
    }
}

/// Parse the data field of an SDC into `info` (in and out); entity types that are not interpreted go to `other` (`info.n_other` of them). Returns the entities read.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_sdc_parse(d: *const u8, n: usize, info: *mut SdcInfo, other: *mut u8, ocap: usize) -> i32 {
    if info.is_null() {
        return 0;
    }
    sdc::sdc_parse(rd(d, n), &mut *info, wr(other, ocap))
}

/// CRC check of a decoded SDC block of `4 + 8 * field_bytes + 16` bits; the AFS index is written.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_sdc_check_bits(b: *const u8, n: usize, field_bytes: usize, afs: *mut i32) -> bool {
    let (a, ok) = sdc::sdc_check_bits(rd(b, n), field_bytes);
    if !afs.is_null() {
        *afs = a;
    }
    ok
}

/// Audio frames per super frame (5, 10 or 0).
#[no_mangle]
pub extern "C" fn onair_drm_aac_num_frames(mode_e: u32, rate_hz: i32) -> i32 {
    let mode_e = mode_e != 0;
    aac::num_frames(mode_e, rate_hz)
}

/// Bytes of the frame border header.
#[no_mangle]
pub extern "C" fn onair_drm_aac_header_bytes(n: i32) -> i32 {
    aac::header_bytes(n)
}

/// Split a logical frame into audio frames (their bytes one after the other in `frames`, `n` bytes of the logical frame are enough).
#[no_mangle]
pub unsafe extern "C" fn onair_drm_aac_super_frame_parse(
    lf: *const u8,
    n: usize,
    len_a: i32,
    frames_n: i32,
    info: *mut AacInfo,
    frames: *mut u8,
    cap: usize,
) -> bool {
    if info.is_null() {
        return false;
    }
    aac::super_frame_parse(rd(lf, n), len_a, frames_n, &mut *info, wr(frames, cap))
}

/// Run a name function.
unsafe fn name(s: &[u8], out: *mut u8, cap: usize) -> usize {
    let o = wr(out, cap);
    let k = s.len().min(o.len());
    o[..k].copy_from_slice(&s[..k]);
    k
}

/// Language name (table 18); 24 bytes are enough.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_language_name(c: i32, out: *mut u8, cap: usize) -> usize {
    name(names::language_name(c), out, cap)
}

/// Programme type name (table 19); 24 bytes are enough.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_programme_type_name(c: i32, out: *mut u8, cap: usize) -> usize {
    name(names::programme_type_name(c), out, cap)
}

/// MSC mode name; 16 bytes are enough.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_msc_mode_name(mode_e: u32, m: i32, out: *mut u8, cap: usize) -> usize {
    let mode_e = mode_e != 0;
    name(names::msc_mode_name(mode_e, m), out, cap)
}

/// Spectrum occupancy name; 16 bytes are enough.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_occupancy_name(mode_e: u32, o: i32, out: *mut u8, cap: usize) -> usize {
    let mode_e = mode_e != 0;
    name(names::occupancy_name(mode_e, o), out, cap)
}

/// Application name; 32 bytes are enough.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_app_name(domain: i32, app: i32, out: *mut u8, cap: usize) -> usize {
    name(names::app_name(domain, app), out, cap)
}

/// Number of audio services for a FAC code (-1 reserved).
#[no_mangle]
pub extern "C" fn onair_drm_fac_audio_services(c: i32) -> i32 {
    names::fac_audio_services(c)
}

/// Number of data services for a FAC code (-1 reserved).
#[no_mangle]
pub extern "C" fn onair_drm_fac_data_services(c: i32) -> i32 {
    names::fac_data_services(c)
}

/// Calendar date of an MJD.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_mjd_to_date(mjd: i32, y: *mut i32, m: *mut i32, d: *mut i32) {
    if y.is_null() || m.is_null() || d.is_null() {
        return;
    }
    let (a, b, c) = data::mjd_to_date(mjd);
    *y = a;
    *m = b;
    *d = c;
}

/// MJD of a calendar date.
#[no_mangle]
pub extern "C" fn onair_drm_date_to_mjd(y: i32, m: i32, d: i32) -> i32 {
    data::date_to_mjd(y, m, d)
}

/// Payload of an MSC data group: offset and length; false when damaged.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_group_payload(d: *const u8, n: usize, off: *mut usize, len: *mut usize) -> bool {
    match data::group_payload(rd(d, n)) {
        Some((o, l)) if !off.is_null() && !len.is_null() => {
            *off = o;
            *len = l;
            true
        }
        _ => false,
    }
}

/// Text of an object for display (needs twice the input length, at most 32768 bytes); returns the length.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_display_text(b: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    data::display_text(rd(b, n), wr(out, cap))
}

/// Does the object look like text?
#[no_mangle]
pub unsafe extern "C" fn onair_drm_looks_text(b: *const u8, n: usize) -> bool {
    data::looks_text(rd(b, n))
}

/// Bytes a text message segment needs, from its first byte.
#[no_mangle]
pub extern "C" fn onair_drm_text_segment_need(b0: u32) -> i32 {
    let b0 = b0 as u8;
    text::segment_need(b0)
}

/// Check a text message segment of `need` bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_drm_text_segment_parse(seg: *const u8, n: usize, need: i32, out: *mut TextSeg) {
    if !out.is_null() {
        *out = text::segment_parse(rd(seg, n), need);
    }
}
