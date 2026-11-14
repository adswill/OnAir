// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_dvbsi` (core/include/dect2/rs/dvbsi.h). Every export is `onair_dvbsi_*`; pointers come as (pointer, length) pairs and the caller owns all
//! buffers. Text functions write UTF-8 into a caller buffer and return the length, `usize::MAX` when it is too small (sizes are documented in the header).
//! Record arrays are (pointer, capacity); parsers count entries past the capacity without storing them. A null pointer where a buffer is needed makes a
//! function fail or do nothing. The iconv callback of `dvb_text` is registered with `onair_dvbsi_set_convert`.
#![allow(clippy::missing_safety_doc)]

use crate::charset::{self, Out};
use crate::subpix;
use crate::subs::{self, ClutEntry, ObjectInfo, PageInfo, PageRegion, PesInfo, RegionInfo, RegionObject, Segment};
use crate::teletext::{self, Unit};
use crate::ttxrender::{self, Cell, COLS, ROWS};
use core::sync::atomic::{AtomicUsize, Ordering};

/// Input slice (empty for a null pointer or length 0).
unsafe fn rd<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(p, n)
    }
}

/// Output slice (empty for a null pointer or capacity 0).
unsafe fn wr<'a, T>(p: *mut T, n: usize) -> &'a mut [T] {
    if p.is_null() || n == 0 {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(p, n)
    }
}

/// The signature of the registered conversion routine: (charset name, NUL terminated; source; length; output; capacity) -> bytes written.
type ConvertFn = unsafe extern "C" fn(*const u8, *const u8, usize, *mut u8, usize) -> usize;

/// The registered conversion routine, 0 when none.
static CONVERT: AtomicUsize = AtomicUsize::new(0);

/// Register the character set conversion routine used by `onair_dvbsi_dvb_text` for the tables that need iconv (null: keep the raw bytes).
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_set_convert(f: Option<ConvertFn>) {
    CONVERT.store(f.map_or(0, |f| f as usize), Ordering::SeqCst);
}

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_dvbsi_version() -> u32 {
    1
}

/// sizeof of the record structs in the order TtxUnit, TtxCell, SubPesInfo, SubSegment, SubPageInfo, SubPageRegion, SubRegionInfo, SubRegionObject, SubClutEntry, SubObjectInfo.
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_struct_sizes(out: *mut usize) {
    if out.is_null() {
        return;
    }
    let v = [
        core::mem::size_of::<Unit>(),
        core::mem::size_of::<Cell>(),
        core::mem::size_of::<PesInfo>(),
        core::mem::size_of::<Segment>(),
        core::mem::size_of::<PageInfo>(),
        core::mem::size_of::<PageRegion>(),
        core::mem::size_of::<RegionInfo>(),
        core::mem::size_of::<RegionObject>(),
        core::mem::size_of::<ClutEntry>(),
        core::mem::size_of::<ObjectInfo>(),
    ];
    for (i, x) in v.iter().enumerate() {
        *out.add(i) = *x;
    }
}

/// Run a text function that writes into an `Out`; the length or `usize::MAX` on overflow.
unsafe fn text(out: *mut u8, cap: usize, f: impl FnOnce(&mut Out)) -> usize {
    let mut o = Out::new(wr(out, cap));
    f(&mut o);
    if o.overflow {
        usize::MAX
    } else {
        o.len()
    }
}

/// EBU Latin to UTF-8 (needs 3 bytes per input byte).
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_ebu_latin(p: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    text(out, cap, |o| charset::ebu_latin(rd(p, n), o))
}

/// UCS-2 big endian to UTF-8 (needs 3 bytes per input byte to be safe).
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_ucs2be(p: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    text(out, cap, |o| charset::ucs2be(rd(p, n), o))
}

/// DAB charset field to UTF-8 (needs 3 bytes per input byte).
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_dab_charset(cs: i32, p: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    text(out, cap, |o| charset::dab_charset(cs, rd(p, n), o))
}

/// ISO/IEC 6937 to UTF-8 (needs 3 bytes per input byte).
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_iso6937(p: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    text(out, cap, |o| charset::iso6937(rd(p, n), o))
}

/// `clean` of UTF-8 text (needs n bytes).
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_clean(p: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    charset::clean(rd(p, n), wr(out, cap))
}

/// A DVB text field to clean UTF-8 (needs 4 * n + 16 bytes).
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_dvb_text(p: *const u8, n: usize, gb_default: u32, out: *mut u8, cap: usize) -> usize {
    onair_dvbsi_dvb_text_with_encoding(p, n, if gb_default != 0 { 1 } else { 0 }, out, cap)
}

/// A DVB field with a generic unprefixed fallback (see charset::Encoding).
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_dvb_text_with_encoding(p: *const u8, n: usize, encoding: u32, out: *mut u8, cap: usize) -> usize {
    let conv = |name: &[u8], src: &[u8], dst: &mut [u8]| -> Option<usize> {
        let f = CONVERT.load(Ordering::SeqCst);
        if f == 0 {
            return None;
        }
        // SAFETY: the value was stored from a `ConvertFn` by `onair_dvbsi_set_convert`.
        let f: ConvertFn = core::mem::transmute::<usize, ConvertFn>(f);
        let mut nm = [0u8; 40];
        let l = name.len().min(39);
        nm[..l].copy_from_slice(&name[..l]);
        let r = f(nm.as_ptr(), src.as_ptr(), src.len(), dst.as_mut_ptr(), dst.len());
        if r == usize::MAX {
            None
        } else {
            Some(r.min(dst.len()))
        }
    };
    charset::dvb_text(rd(p, n), charset::Encoding::from_code(encoding), Some(&conv), wr(out, cap))
}

/// Hamming 8/4 decode: nibble or -1.
#[no_mangle]
pub extern "C" fn onair_dvbsi_ttx_hamming84(b: u32) -> i32 {
    let b = b as u8;
    teletext::hamming84(b)
}

/// Hamming 24/18 decode of three bytes: 18 data bits or -1.
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_ttx_hamming2418(b3: *const u8) -> i32 {
    let s = rd(b3, 3);
    if s.len() < 3 {
        return -1;
    }
    teletext::hamming2418(&[s[0], s[1], s[2]])
}

/// Bit reversal of a byte.
#[no_mangle]
pub extern "C" fn onair_dvbsi_ttx_reverse8(b: u32) -> u8 {
    let b = b as u8;
    teletext::reverse8(b)
}

/// G0 code to Unicode in a national option subset.
#[no_mangle]
pub extern "C" fn onair_dvbsi_ttx_g0(code: u32, national: i32) -> u32 {
    let code = code as u8;
    teletext::g0(code, national)
}

/// Offsets of the 44 byte teletext data units of a PES packet; returns the number found (counted past the capacity, stored up to it).
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_ttx_pes_units(pes: *const u8, n: usize, out: *mut u32, cap: usize) -> usize {
    teletext::pes_units(rd(pes, n), wr(out, cap))
}

/// Decode a 44 byte data unit.
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_ttx_decode_unit(u44: *const u8, out: *mut Unit) {
    if out.is_null() {
        return;
    }
    let s = rd(u44, 44);
    *out = if s.len() < 44 {
        Unit::ignore()
    } else {
        let mut a = [0u8; 44];
        a.copy_from_slice(s);
        teletext::decode_unit(&a)
    };
}

/// Render a page: 25 x 40 characters, the received-rows mask and the national subset to 25 x 40 cells.
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_ttx_render(rows: *const u8, row_mask: u32, national: i32, out: *mut Cell) {
    let r = rd(rows, ROWS * COLS);
    if r.len() < ROWS * COLS || out.is_null() {
        return;
    }
    let mut a = [0u8; ROWS * COLS];
    a.copy_from_slice(r);
    ttxrender::render(&a, row_mask, national, &mut *(out as *mut [Cell; ROWS * COLS]));
}

/// UTF-8 text of one rendered row (needs 120 bytes).
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_ttx_row_text(cells: *const Cell, row: i32, out: *mut u8, cap: usize) -> usize {
    if cells.is_null() {
        return 0;
    }
    ttxrender::row_text(&*(cells as *const [Cell; ROWS * COLS]), row, wr(out, cap))
}

/// Header of a DVB subtitle PES packet; false when it is not one.
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_sub_pes_info(pes: *const u8, n: usize, out: *mut PesInfo) -> bool {
    match subs::pes_info(rd(pes, n)) {
        Some(i) if !out.is_null() => {
            *out = i;
            true
        }
        _ => false,
    }
}

/// The segments of a subtitle PES payload; the number found (counted past the capacity).
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_sub_segments(d: *const u8, n: usize, out: *mut Segment, cap: usize) -> usize {
    subs::segments(rd(d, n), wr(out, cap))
}

/// Page composition segment; false when too short.
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_sub_page_composition(
    d: *const u8,
    n: usize,
    info: *mut PageInfo,
    out: *mut PageRegion,
    cap: usize,
) -> bool {
    match subs::page_composition(rd(d, n), wr(out, cap)) {
        Some(i) if !info.is_null() => {
            *info = i;
            true
        }
        _ => false,
    }
}

/// Region composition segment; false when too short.
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_sub_region_composition(
    d: *const u8,
    n: usize,
    info: *mut RegionInfo,
    out: *mut RegionObject,
    cap: usize,
) -> bool {
    match subs::region_composition(rd(d, n), wr(out, cap)) {
        Some(i) if !info.is_null() => {
            *info = i;
            true
        }
        _ => false,
    }
}

/// CLUT definition segment: the CLUT id and the number of entries (counted past the capacity); `usize::MAX` when too short.
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_sub_clut_definition(d: *const u8, n: usize, id: *mut i32, out: *mut ClutEntry, cap: usize) -> usize {
    match subs::clut_definition(rd(d, n), wr(out, cap)) {
        Some((i, c)) if !id.is_null() => {
            *id = i;
            c
        }
        _ => usize::MAX,
    }
}

/// Display definition segment: width and height (720 x 576 when out of range); false when too short.
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_sub_display_definition(d: *const u8, n: usize, w: *mut i32, h: *mut i32) -> bool {
    match subs::display_definition(rd(d, n)) {
        Some((a, b)) if !w.is_null() && !h.is_null() => {
            *w = a;
            *h = b;
            true
        }
        _ => false,
    }
}

/// Object data segment of a bitmap object; false for other objects or a bad length.
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_sub_object_data(d: *const u8, n: usize, out: *mut ObjectInfo) -> bool {
    match subs::object_data(rd(d, n)) {
        Some(i) if !out.is_null() => {
            *out = i;
            true
        }
        _ => false,
    }
}

/// The default colour tables.
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_sub_default_clut(e2: *mut u32, e4: *mut u32, e8: *mut u32) {
    if e2.is_null() || e4.is_null() || e8.is_null() {
        return;
    }
    subpix::default_clut(&mut *(e2 as *mut [u32; 4]), &mut *(e4 as *mut [u32; 16]), &mut *(e8 as *mut [u32; 256]));
}

/// YCrCbT to RGBA.
#[no_mangle]
pub extern "C" fn onair_dvbsi_sub_ycrcb(y: i32, cr: i32, cb: i32, t: i32) -> u32 {
    subpix::ycrcb(y, cr, cb, t)
}

/// Decode one field's pixel data into a region (`pix` has `reg_w * reg_h` bytes).
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn onair_dvbsi_sub_decode_field(
    f: *const u8,
    n: usize,
    x0: i32,
    y0: i32,
    line_step: i32,
    double_lines: u32,
    reg_w: i32,
    reg_h: i32,
    reg_depth: i32,
    pix: *mut u8,
) {
    let double_lines = double_lines != 0;
    if pix.is_null() || reg_w <= 0 || reg_h <= 0 {
        return;
    }
    let len = reg_w as usize * reg_h as usize;
    subpix::decode_field(rd(f, n), x0, y0, line_step, double_lines, reg_w, reg_h, reg_depth, wr(pix, len));
}

/// Palette indices to RGBA through the CLUT of the region depth (`out` has 4 * n bytes).
#[no_mangle]
pub unsafe extern "C" fn onair_dvbsi_sub_region_rgba(
    pix: *const u8,
    n: usize,
    depth: i32,
    e2: *const u32,
    e4: *const u32,
    e8: *const u32,
    out: *mut u8,
) {
    if e2.is_null() || e4.is_null() || e8.is_null() || out.is_null() {
        return;
    }
    subpix::region_rgba(
        rd(pix, n),
        depth,
        &*(e2 as *const [u32; 4]),
        &*(e4 as *const [u32; 16]),
        &*(e8 as *const [u32; 256]),
        wr(out, n * 4),
    );
}

/// Blend a region into a display picture (`dw * dh * 4` bytes, `rw * rh * 4` bytes of region).
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn onair_dvbsi_sub_blend_region(out: *mut u8, dw: i32, dh: i32, rx: i32, ry: i32, rw: i32, rh: i32, rgba: *const u8) {
    if dw <= 0 || dh <= 0 || rw <= 0 || rh <= 0 {
        return;
    }
    let o = wr(out, dw as usize * dh as usize * 4);
    subpix::blend_region(o, dw, dh, rx, ry, rw, rh, rd(rgba, rw as usize * rh as usize * 4));
}

/// Distance a - b on the 33-bit PTS circle in seconds.
#[no_mangle]
pub extern "C" fn onair_dvbsi_sub_pts_delta(a: f64, b: f64) -> f64 {
    subpix::pts_delta(a, b)
}
