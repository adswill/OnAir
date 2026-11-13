// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_ax25` (core/include/dect2/rs/ax25.h). Every export is `onair_ax25_*`; pointers come as (pointer, length) pairs and the caller owns all
//! buffers. Text writers return the length written, or `usize::MAX` when the buffer is too small. Out parameters are written only on success unless noted.
//!
//! This is also the only module with `unsafe`: besides the C ABI it holds `cfn`, the two C library functions (`pow`, `snprintf`) that `no_std` Rust has
//! no equivalent of with bit-identical results.
#![allow(clippy::missing_safety_doc)]

use crate::aprs::{self, Info};
use crate::arena::Arena;
use crate::frame::{self, AddrRef, Frame};
use crate::hdlc::{Event, Hdlc};

const FAIL: usize = usize::MAX;

/// C library functions: `pow` (the C++ reference calls the same one, so results are identical) and `snprintf("%.Nf")` for the digits of numbers in text.
pub(crate) mod cfn {
    use core::ffi::{c_char, c_int};
    mod c {
        use core::ffi::{c_char, c_int};
        extern "C" {
            pub fn pow(x: f64, y: f64) -> f64;
            pub fn snprintf(s: *mut c_char, n: usize, fmt: *const c_char, ...) -> c_int;
        }
    }
    /// `pow(x, y)` of the C library.
    pub fn pow(x: f64, y: f64) -> f64 {
        // SAFETY: a pure C library function of two doubles.
        unsafe { c::pow(x, y) }
    }
    /// Writes `v` as C `%.{prec}f` (prec 0 to 4) into `out` (64 bytes, like the reference's buffer); returns the length (at most 63).
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
pub extern "C" fn onair_ax25_version() -> u32 {
    2
}

/// sizeof of the shared structs (frame, address-in, APRS info, HDLC state): the C++ side checks them at start-up.
#[no_mangle]
pub unsafe extern "C" fn onair_ax25_struct_sizes(frame: *mut usize, addr_in: *mut usize, info: *mut usize, hdlc: *mut usize) {
    for (p, v) in [
        (frame, core::mem::size_of::<Frame>()),
        (addr_in, core::mem::size_of::<AddrIn>()),
        (info, core::mem::size_of::<Info>()),
        (hdlc, core::mem::size_of::<Hdlc>()),
    ] {
        if !p.is_null() {
            *p = v;
        }
    }
}

/// The AX.25 frame check sequence (CRC-16/X-25) of the bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_ax25_fcs(d: *const u8, n: usize) -> u16 {
    frame::fcs(rd(d, n))
}

/// True when the last two bytes are the FCS of the bytes before them.
#[no_mangle]
pub unsafe extern "C" fn onair_ax25_fcs_ok(d: *const u8, n: usize) -> bool {
    frame::fcs_ok(rd(d, n))
}

/// Parses a frame (without FCS) into `*out` (an `OnairAx25Frame`); false when malformed (`*out` untouched).
#[no_mangle]
pub unsafe extern "C" fn onair_ax25_parse_frame(d: *const u8, n: usize, out: *mut Frame) -> bool {
    match frame::parse_frame(rd(d, n)) {
        Some(f) if !out.is_null() => {
            *out = f;
            true
        }
        Some(_) => true,
        None => false,
    }
}

/// An address handed in for text building (callsign pointer and length, SSID, H bit).
#[repr(C)]
pub struct AddrIn {
    call: *const u8,
    call_len: usize,
    ssid: i32,
    h: u8,
}

unsafe fn addr_ref<'a>(a: &AddrIn) -> AddrRef<'a> {
    AddrRef { call: rd(a.call, a.call_len), ssid: a.ssid, h: a.h != 0 }
}

/// Fixed-size scratch for a path (digipeater lists above this are cut: AX.25 allows eight, the parser stops at eight).
const PATH_MAX: usize = 64;

unsafe fn path_refs<'a>(p: *const AddrIn, n: usize, tmp: &mut [Option<AddrRef<'a>>; PATH_MAX]) -> usize {
    let n = if p.is_null() { 0 } else { n.min(PATH_MAX) };
    for (i, t) in tmp.iter_mut().enumerate().take(n) {
        *t = Some(addr_ref(&*p.add(i)));
    }
    n
}

fn collect<'a>(tmp: &[Option<AddrRef<'a>>; PATH_MAX], n: usize, out: &mut [AddrRef<'a>; PATH_MAX]) {
    for i in 0..n {
        if let Some(a) = tmp[i] {
            out[i] = a;
        }
    }
}

/// `CALL` or `CALL-SSID`.
#[no_mangle]
pub unsafe extern "C" fn onair_ax25_addr_str(a: *const AddrIn, out: *mut u8, cap: usize) -> usize {
    if a.is_null() {
        return 0;
    }
    let mut o = Arena::new(wr(out, cap));
    frame::put_addr(&mut o, &addr_ref(&*a));
    if o.overflow {
        FAIL
    } else {
        o.mark()
    }
}

/// "WIDE1-1*,WIDE2-1" for `n` digipeaters.
#[no_mangle]
pub unsafe extern "C" fn onair_ax25_path_str(path: *const AddrIn, n: usize, out: *mut u8, cap: usize) -> usize {
    let mut tmp: [Option<AddrRef>; PATH_MAX] = [None; PATH_MAX];
    let mut refs = [AddrRef { call: &[], ssid: 0, h: false }; PATH_MAX];
    let n = path_refs(path, n, &mut tmp);
    collect(&tmp, n, &mut refs);
    let mut o = Arena::new(wr(out, cap));
    frame::put_path(&mut o, &refs[..n]);
    if o.overflow {
        FAIL
    } else {
        o.mark()
    }
}

/// The TNC-2 line `FROM>TO,PATH:info`.
#[no_mangle]
pub unsafe extern "C" fn onair_ax25_tnc2(
    from: *const AddrIn,
    to: *const AddrIn,
    path: *const AddrIn,
    n: usize,
    info: *const u8,
    info_len: usize,
    out: *mut u8,
    cap: usize,
) -> usize {
    if from.is_null() || to.is_null() {
        return 0;
    }
    let mut tmp: [Option<AddrRef>; PATH_MAX] = [None; PATH_MAX];
    let mut refs = [AddrRef { call: &[], ssid: 0, h: false }; PATH_MAX];
    let n = path_refs(path, n, &mut tmp);
    collect(&tmp, n, &mut refs);
    let mut o = Arena::new(wr(out, cap));
    frame::put_tnc2(&mut o, &addr_ref(&*from), &addr_ref(&*to), &refs[..n], rd(info, info_len));
    if o.overflow {
        FAIL
    } else {
        o.mark()
    }
}

/// Frame type name ("UI", "RR", ...) from the control byte.
#[no_mangle]
pub unsafe extern "C" fn onair_ax25_type_name(control: u32, out: *mut u8, cap: usize) -> usize {
    let control = control as u8;
    let mut o = Arena::new(wr(out, cap));
    o.put(frame::type_name(control).as_bytes());
    if o.overflow {
        FAIL
    } else {
        o.mark()
    }
}

/// Resets an HDLC receiver state.
#[no_mangle]
pub unsafe extern "C" fn onair_ax25_hdlc_reset(h: *mut Hdlc) {
    if !h.is_null() {
        (*h).reset();
    }
}

/// Feeds one bit (NRZI level when `nrzi`, plain HDLC bit otherwise). Returns 0 nothing, 1 good frame (`buf[..*len]` without FCS), 2 bad FCS (`buf[..*len]` every byte).
#[no_mangle]
pub unsafe extern "C" fn onair_ax25_hdlc_bit(h: *mut Hdlc, level: i32, nrzi: u32, len: *mut usize) -> i32 {
    let nrzi = nrzi != 0;
    if h.is_null() {
        return 0;
    }
    let (code, n) = match (*h).bit(level, nrzi) {
        Event::None => (0, 0),
        Event::Good(n) => (1, n),
        Event::Bad(n) => (2, n),
    };
    if !len.is_null() {
        *len = n;
    }
    code
}

/// Parses an APRS information field. Returns 1 true, 0 false, -1 when the text buffer (`cap` bytes) was too small. `*info_out` (an `OnairAprsInfo`) is
/// always filled as far as the parser got; its strings are (offset, length) pairs into `text`.
#[no_mangle]
pub unsafe extern "C" fn onair_ax25_aprs_parse(
    dest: *const u8,
    dest_len: usize,
    info: *const u8,
    info_len: usize,
    info_out: *mut Info,
    text: *mut u8,
    cap: usize,
) -> i32 {
    if info_out.is_null() {
        return 0;
    }
    let mut a = Arena::new(wr(text, cap));
    let ok = aprs::parse(rd(dest, dest_len), rd(info, info_len), &mut *info_out, &mut a);
    if a.overflow {
        -1
    } else {
        i32::from(ok)
    }
}

/// Packet type name of a type id ("Position", ...).
#[no_mangle]
pub unsafe extern "C" fn onair_ax25_aprs_type_name(id: i32, out: *mut u8, cap: usize) -> usize {
    let mut o = Arena::new(wr(out, cap));
    if let Some(s) = usize::try_from(id).ok().and_then(|i| aprs::TYPE_NAMES.get(i)) {
        o.put(s.as_bytes());
    }
    if o.overflow {
        FAIL
    } else {
        o.mark()
    }
}

/// Latitude as "25.2000N".
#[no_mangle]
pub unsafe extern "C" fn onair_ax25_aprs_fmt_lat(v: f64, out: *mut u8, cap: usize) -> usize {
    let mut o = Arena::new(wr(out, cap));
    aprs::put_lat(&mut o, v);
    if o.overflow {
        FAIL
    } else {
        o.mark()
    }
}

/// Longitude as "55.3000E".
#[no_mangle]
pub unsafe extern "C" fn onair_ax25_aprs_fmt_lon(v: f64, out: *mut u8, cap: usize) -> usize {
    let mut o = Arena::new(wr(out, cap));
    aprs::put_lon(&mut o, v);
    if o.overflow {
        FAIL
    } else {
        o.mark()
    }
}
