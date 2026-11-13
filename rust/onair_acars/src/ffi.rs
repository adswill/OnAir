// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_acars` (core/include/dect2/rs/acars.h). Every export is `onair_acars_*`; pointers come as (pointer, length) pairs and the caller
//! owns all buffers. Functions that write text return the length written, or `usize::MAX` when the buffer is too small.
#![allow(clippy::missing_safety_doc)]

use crate::block;
use crate::text::Buf;
use crate::{crc, label, repair};

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
pub extern "C" fn onair_acars_version() -> u32 {
    1
}

/// sizeof(OnairAcarsBlock): the C++ side checks it against its own struct when the program starts.
#[no_mangle]
pub extern "C" fn onair_acars_block_size() -> usize {
    core::mem::size_of::<OnairAcarsBlock>()
}

#[no_mangle]
pub extern "C" fn onair_acars_crc_update(c: u32, b: u32) -> u16 {
    let c = c as u16;
    let b = b as u8;
    crc::update(c, b)
}
#[no_mangle]
pub unsafe extern "C" fn onair_acars_crc(p: *const u8, n: usize, c: u32) -> u16 {
    let c = c as u16;
    crc::crc(rd(p, n), c)
}
#[no_mangle]
pub extern "C" fn onair_acars_syndrome(pos_from_end: i32, bit: i32) -> u16 {
    crc::syndrome(pos_from_end, bit)
}

/// Parity and check-sequence repair of `txt` (len bytes, in place) with the two check bytes; returns true when the block is good.
#[no_mangle]
pub unsafe extern "C" fn onair_acars_repair_block(
    txt: *mut u8,
    len: usize,
    crc0: u32,
    crc1: u32,
    max_parity_errors: i32,
    fixed_bits: *mut i32,
    parity_errors: *mut i32,
) -> bool {
    let crc0 = crc0 as u8;
    let crc1 = crc1 as u8;
    let r = repair::repair_block(wr(txt, len), [crc0, crc1], max_parity_errors);
    if !fixed_bits.is_null() {
        *fixed_bits = r.fixed_bits;
    }
    if !parity_errors.is_null() {
        *parity_errors = r.parity_errors;
    }
    r.ok
}

/// Label name as a static NUL-terminated string ("" when unknown); `a` and `b` are the two label characters (the DEL written as 'd').
#[no_mangle]
pub extern "C" fn onair_acars_label_text(a: u32, b: u32) -> *const u8 {
    let a = a as u8;
    let b = b as u8;
    label::label_text(a, b).as_ptr()
}

/// Short reading of fixed-format text; returns the length written, SIZE_MAX when `out` is too small.
#[no_mangle]
pub unsafe extern "C" fn onair_acars_decode_text(l: *const u8, ln: usize, t: *const u8, tn: usize, out: *mut u8, cap: usize) -> usize {
    let mut b = Buf::new(wr(out, cap));
    label::decode_text(rd(l, ln), rd(t, tn), &mut b);
    if b.overflow {
        usize::MAX
    } else {
        b.len()
    }
}

/// The fields of a parsed block; see `block::Fields`. `stage`: 0 rejected, 1 header only, 2 no text, 3 complete.
#[repr(C)]
pub struct OnairAcarsBlock {
    pub stage: u8,
    pub final_block: u8,
    pub mode: u8,
    pub reg_len: u8,
    pub reg: [u8; 7],
    pub ack: u8,
    pub label: [u8; 2],
    pub block_id: u8,
    pub downlink: u8,
    pub has_msg_num: u8,
    pub msg_num: [u8; 3],
    pub msg_seq: u8,
    pub flight_id: [u8; 6],
    pub sublabel_len: u8,
    pub sublabel: [u8; 2],
    pub mfi_len: u8,
    pub mfi: [u8; 2],
    pub text_len: u16,
    pub decoded_len: u16,
    pub text: [u8; block::TEXT_CAP],
    pub decoded: [u8; block::DECODED_CAP],
}

/// Parses a good block (bytes after SOH up to and including the suffix, parity removed). Returns the stage reached (see `OnairAcarsBlock`).
#[no_mangle]
pub unsafe extern "C" fn onair_acars_parse_block(t: *const u8, len: usize, m: *mut OnairAcarsBlock) -> i32 {
    if m.is_null() {
        return 0;
    }
    let m = &mut *m;
    let (f, dl) = block::parse(rd(t, len), &mut m.decoded);
    m.stage = f.stage as u8;
    m.final_block = f.final_block as u8;
    m.mode = f.mode;
    m.reg_len = f.reg_len as u8;
    m.reg = f.reg;
    m.ack = f.ack;
    m.label = f.label;
    m.block_id = f.block_id;
    m.downlink = f.downlink as u8;
    m.has_msg_num = f.has_msg_num as u8;
    m.msg_num = f.msg_num;
    m.msg_seq = f.msg_seq;
    m.flight_id = f.flight_id;
    m.sublabel_len = f.sublabel_len as u8;
    m.sublabel = f.sublabel;
    m.mfi_len = f.mfi_len as u8;
    m.mfi = f.mfi;
    m.text_len = f.text_len as u16;
    m.decoded_len = dl as u16;
    m.text = f.text;
    f.stage as i32
}
