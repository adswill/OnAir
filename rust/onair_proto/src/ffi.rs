// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The C ABI (core/include/dect2/rs/proto.h). The only file with `unsafe`: it turns (pointer, length) pairs into slices and calls the safe
//! modules. Callers own every buffer; functions that write return the length written, or `usize::MAX` when the buffer is too small.
#![allow(clippy::missing_safety_doc)]

use crate::{bits, frame, mmsi, msg, nmea};

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

// The panic handler and the unwinding personality stub live once, in the aggregating staticlib crate rust/onair.

#[no_mangle]
pub extern "C" fn onair_proto_version() -> u32 {
    1
}

/// sizeof(OnairAisMsg): the C++ side checks it against its own struct when the program starts.
#[no_mangle]
pub extern "C" fn onair_ais_msg_size() -> usize {
    core::mem::size_of::<msg::Msg>()
}

#[no_mangle]
pub unsafe extern "C" fn onair_ais_get_u(b: *const u8, n: usize, pos: usize, len: usize) -> u32 {
    bits::get_u(rd(b, n), pos, len)
}
#[no_mangle]
pub unsafe extern "C" fn onair_ais_get_s(b: *const u8, n: usize, pos: usize, len: usize) -> i32 {
    bits::get_s(rd(b, n), pos, len)
}
/// Text of `len` bits (at most len / 6 characters are written to `out`).
#[no_mangle]
pub unsafe extern "C" fn onair_ais_get_text(b: *const u8, n: usize, pos: usize, len: usize, out: *mut u8, cap: usize) -> usize {
    bits::get_text(rd(b, n), pos, len, wr(out, cap))
}

#[no_mangle]
pub unsafe extern "C" fn onair_ais_fcs_register(b: *const u8, n: usize) -> u16 {
    frame::fcs_register(rd(b, n))
}
#[no_mangle]
pub unsafe extern "C" fn onair_ais_fcs_ok(b: *const u8, n: usize) -> bool {
    frame::fcs_ok(rd(b, n))
}
/// `out` needs n + n / 5 + 1 bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_ais_stuff(b: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    frame::stuff(rd(b, n), wr(out, cap)).unwrap_or(FAIL)
}
/// `out` needs n bytes. Returns `usize::MAX` when six 1s in a row appear.
#[no_mangle]
pub unsafe extern "C" fn onair_ais_destuff(b: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    frame::destuff(rd(b, n), wr(out, cap)).unwrap_or(FAIL)
}
#[no_mangle]
pub unsafe extern "C" fn onair_ais_nrzi_encode(b: *const u8, n: usize, start: i32, out: *mut u8, cap: usize) -> usize {
    frame::nrzi_encode(rd(b, n), start, wr(out, cap)).unwrap_or(FAIL)
}
#[no_mangle]
pub unsafe extern "C" fn onair_ais_nrzi_decode(b: *const u8, n: usize, prev: i32, out: *mut u8, cap: usize) -> usize {
    frame::nrzi_decode(rd(b, n), prev, wr(out, cap)).unwrap_or(FAIL)
}
#[no_mangle]
pub unsafe extern "C" fn onair_ais_reverse_octets(b: *mut u8, n: usize) {
    frame::reverse_octets(wr(b, n))
}
/// Good frames are written one after the other to `out` (`out_cap >= n` bits) with their lengths in `lens` (`lens_cap >= n / 64 + 2`).
/// Returns the number of good frames; `*bad` gets the count of failed bursts.
#[no_mangle]
pub unsafe extern "C" fn onair_ais_hdlc_frames(
    b: *const u8,
    n: usize,
    out: *mut u8,
    out_cap: usize,
    lens: *mut usize,
    lens_cap: usize,
    bad: *mut i32,
) -> usize {
    let lens_s: &mut [usize] = if lens.is_null() || lens_cap == 0 { &mut [] } else { core::slice::from_raw_parts_mut(lens, lens_cap) };
    let (good, nbad) = frame::hdlc_frames(rd(b, n), wr(out, out_cap), lens_s);
    if !bad.is_null() {
        *bad = nbad;
    }
    good
}

#[no_mangle]
pub unsafe extern "C" fn onair_ais_decode_message(b: *const u8, n: usize, m: *mut msg::Msg) -> bool {
    if m.is_null() {
        return false;
    }
    msg::decode_message(rd(b, n), &mut *m)
}

#[no_mangle]
pub extern "C" fn onair_ais_mmsi_plausible(m: u32) -> bool {
    mmsi::mmsi_plausible(m)
}
#[no_mangle]
pub extern "C" fn onair_ais_acceptable(typ: i32, valid: u32, mmsi: u32, decoded: u32) -> bool {
    let valid = valid != 0;
    let decoded = decoded != 0;
    mmsi::acceptable_ais(typ, valid, mmsi, decoded)
}
#[no_mangle]
pub extern "C" fn onair_ais_class_of_mmsi(mmsi: u32, typ: i32) -> i32 {
    mmsi::class_of_mmsi(mmsi, typ)
}
#[no_mangle]
pub extern "C" fn onair_ais_rate_of_turn(raw: i32) -> f64 {
    msg::rate_of_turn(raw)
}

/// `out` needs (n + 5) / 6 bytes; `*fill` gets the fill bits.
#[no_mangle]
pub unsafe extern "C" fn onair_ais_armour(b: *const u8, n: usize, out: *mut u8, cap: usize, fill: *mut i32) -> usize {
    match nmea::armour(rd(b, n), wr(out, cap)) {
        Some((len, f)) => {
            if !fill.is_null() {
                *fill = f;
            }
            len
        }
        None => FAIL,
    }
}
/// `out` needs n * 6 bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_ais_unarmour(c: *const u8, n: usize, fill: i32, out: *mut u8, cap: usize) -> usize {
    nmea::unarmour(rd(c, n), fill, wr(out, cap)).unwrap_or(FAIL)
}
#[no_mangle]
pub unsafe extern "C" fn onair_nmea_checksum(c: *const u8, n: usize) -> u8 {
    nmea::checksum(rd(c, n))
}
/// Parses a sentence. Returns 0 (rejected, nothing set), 1 (layout wrong, outputs set as far as the C++ parser sets them) or 2 (good).
/// `chars_start` / `chars_len` locate the payload characters inside the sentence; `channel` may be null.
#[no_mangle]
pub unsafe extern "C" fn onair_nmea_parse(
    s: *const u8,
    n: usize,
    chars_start: *mut usize,
    chars_len: *mut usize,
    fill: *mut i32,
    frag_count: *mut i32,
    frag_no: *mut i32,
    channel: *mut u8,
) -> i32 {
    let r = nmea::parse(rd(s, n));
    if r.state != 0 {
        if !chars_start.is_null() {
            *chars_start = r.chars_start;
        }
        if !chars_len.is_null() {
            *chars_len = r.chars_len;
        }
        if !fill.is_null() {
            *fill = r.fill;
        }
        if !frag_count.is_null() {
            *frag_count = r.frag_count;
        }
        if !frag_no.is_null() {
            *frag_no = r.frag_no;
        }
        if !channel.is_null() {
            *channel = r.channel;
        }
    }
    r.state
}
