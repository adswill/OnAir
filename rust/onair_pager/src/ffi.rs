// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_pager` (core/include/dect2/rs/pager.h). Every export is `onair_pager_*`; pointers come as (pointer, length) pairs and the caller owns all
//! buffers. Text writers return the length written (the buffers are sized by the caller so that nothing is cut). Out parameters are written only when
//! the function reports success, like the C++ reference.
#![allow(clippy::missing_safety_doc)]

use crate::bch;
use crate::flex::{self, Page, PhaseStat};
use crate::pocsag::{self, Word};
use crate::text;

unsafe fn rd<'a>(p: *const u8, n: usize) -> &'a [u8] {
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
unsafe fn put<T>(p: *mut T, v: T) {
    if !p.is_null() {
        *p = v;
    }
}

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_pager_version() -> u32 {
    2
}

/// sizeof(OnairPagerPage), checked by the C++ side at start-up.
#[no_mangle]
pub extern "C" fn onair_pager_page_size() -> usize {
    core::mem::size_of::<Page>()
}

/// 21 data bits to the 32-bit BCH code word (check bits and parity added).
#[no_mangle]
pub extern "C" fn onair_pager_encode_word(data21: u32) -> u32 {
    bch::encode_word(data21)
}

/// Checks and repairs a word. Returns the number of corrected bits (0 to 2) and writes the 21 data bits, or returns -1 when it cannot be repaired.
#[no_mangle]
pub unsafe extern "C" fn onair_pager_correct_word(w: u32, data: *mut u32) -> i32 {
    let f = bch::correct_word(w);
    if !f.ok {
        return -1;
    }
    put(data, f.data);
    f.bits
}

/// Reverses the low 21 bits.
#[no_mangle]
pub extern "C" fn onair_pager_rev21(v: u32) -> u32 {
    bch::rev21(v)
}

/// POCSAG word classification: returns 0 failed, 1 idle, 2 address, 3 data. Address: `a` = top 18 address bits, `b` = function bits; data: `a` = 20 bits.
/// `bits` receives the corrected bit count (0 for failed and idle words that needed no repair).
#[no_mangle]
pub unsafe extern "C" fn onair_pager_pocsag_classify(raw: u32, a: *mut u32, b: *mut u32, bits: *mut i32, fixed: *mut i32) -> i32 {
    let (w, repaired) = pocsag::classify(raw);
    put(fixed, i32::from(repaired));
    match w {
        Word::Failed => 0,
        Word::Idle => 1,
        Word::Address { ric18, func, bits: n } => {
            put(a, ric18);
            put(b, func);
            put(bits, n);
            2
        }
        Word::Data { data20, bits: n } => {
            put(a, data20);
            put(bits, n);
            3
        }
    }
}

/// The message bits (one per byte) read as 7-bit characters look like real text.
#[no_mangle]
pub unsafe extern "C" fn onair_pager_looks_alpha(bits: *const u8, n: usize) -> bool {
    text::looks_alpha(rd(bits, n))
}

/// The bits read as 7-bit characters (printable only); the buffer must hold n / 7 bytes. Returns the length.
#[no_mangle]
pub unsafe extern "C" fn onair_pager_alpha_from_bits(bits: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    text::alpha_from_bits(rd(bits, n), wr(out, cap))
}

/// The bits read as 4-bit digits; `flex` 1 picks the FLEX character set and drops filler 12, 0 the POCSAG one. The buffer must hold n / 4 bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_pager_numeric_from_bits(bits: *const u8, n: usize, flex: u32, out: *mut u8, cap: usize) -> usize {
    let flex = flex != 0;
    let table = if flex { text::NUM_FLEX } else { text::NUM_POCSAG };
    text::numeric_from_bits(rd(bits, n), table, flex, wr(out, cap))
}

/// Index of the FLEX speed table entry closest to `code` within `max_distance` bits, or -1.
#[no_mangle]
pub extern "C" fn onair_pager_flex_mode_for_code(code: u32, max_distance: i32) -> i32 {
    let code = code as u16;
    flex::mode_for_code(code, max_distance).map_or(-1, |i| i as i32)
}

/// Index of the FLEX speed table entry of a `PagerSpeed`, or -1.
#[no_mangle]
pub extern "C" fn onair_pager_flex_mode_for_speed(speed: i32) -> i32 {
    flex::mode_for_speed(speed).map_or(-1, |i| i as i32)
}

/// Number of entries of the FLEX speed table.
#[no_mangle]
pub extern "C" fn onair_pager_flex_mode_count() -> i32 {
    flex::MODES.len() as i32
}

/// Fields of a speed table entry; false for an index out of range.
#[no_mangle]
pub unsafe extern "C" fn onair_pager_flex_mode_info(idx: i32, code: *mut u16, baud: *mut i32, levels: *mut i32, speed: *mut i32) -> bool {
    let Some(m) = usize::try_from(idx).ok().and_then(|i| flex::MODES.get(i)) else {
        return false;
    };
    put(code, m.code);
    put(baud, m.baud);
    put(levels, m.levels);
    put(speed, m.speed);
    true
}

/// The frame information word for a cycle and frame number.
#[no_mangle]
pub extern "C" fn onair_pager_flex_fiw_data(cycle: i32, frame: i32) -> u32 {
    flex::fiw_data(cycle, frame)
}

/// Checks a frame information word; on success writes cycle and frame.
#[no_mangle]
pub unsafe extern "C" fn onair_pager_flex_fiw_check(f: u32, cycle: *mut i32, frame: *mut i32) -> bool {
    match flex::fiw_check(f) {
        Some((c, fr)) => {
            put(cycle, c);
            put(frame, fr);
            true
        }
        None => false,
    }
}

/// Decodes one FLEX phase (2816 bits, one per byte) into at most `cap` pages (`OnairPagerPage`); `stat` is [ok, fixed, failed, biwOk] and is added to.
/// Returns the number of pages.
#[no_mangle]
pub unsafe extern "C" fn onair_pager_flex_decode_phase(bits: *const u8, out: *mut Page, cap: usize, stat: *mut i32) -> usize {
    let mut st = PhaseStat::default();
    let n = flex::decode_phase(rd(bits, flex::PHASE_BITS), wr(out, cap), &mut st);
    if !stat.is_null() {
        *stat += st.ok;
        *stat.add(1) += st.fixed;
        *stat.add(2) += st.failed;
        if st.biw_ok {
            *stat.add(3) = 1;
        }
    }
    n
}
