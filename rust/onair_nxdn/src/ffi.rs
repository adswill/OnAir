// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_nxdn` (core/include/dect2/rs/nxdn.h). Every export is `onair_nxdn_*`; pointers come as (pointer, length) pairs and the caller owns all buffers.
//! Functions with output buffers return false (or `usize::MAX`) when a buffer is too small. Names are static NUL-terminated strings.
#![allow(clippy::missing_safety_doc)]

use crate::chan::{self, Ch};
use crate::crc;
use crate::lich::{self, Lich};
use crate::msg::{self, Msg};

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
pub extern "C" fn onair_nxdn_version() -> u32 {
    2
}

/// A layer 3 message as the C++ side sees it (`text` is `text_len` bytes, no terminator).
#[repr(C)]
pub struct OnairNxdnMsg {
    pub typ: i32,
    pub call_type: i32,
    pub option: i32,
    pub src: i32,
    pub dst: i32,
    pub cipher: i32,
    pub key_id: i32,
    pub status: i32,
    pub block: i32,
    pub blocks: i32,
    pub location: u32,
    pub service: i32,
    pub channel: i32,
    pub text: [u8; 8],
    pub text_len: u32,
}

/// sizeof(OnairNxdnMsg) as Rust sees it; the C++ side checks it at start-up.
#[no_mangle]
pub extern "C" fn onair_nxdn_struct_size() -> usize {
    core::mem::size_of::<OnairNxdnMsg>()
}

/// Writes the PN9 scrambler bits (one per byte) into `out[0..n]`.
#[no_mangle]
pub unsafe extern "C" fn onair_nxdn_pn9(out: *mut u8, n: usize) {
    lich::pn9(wr(out, n));
}

/// LICH byte (8 bits, MSB first, parity last) of the four fields.
#[no_mangle]
pub extern "C" fn onair_nxdn_lich_encode(rfct: i32, fct: i32, opt: i32, dir: i32) -> u8 {
    lich::encode(&Lich { rfct, fct, opt, dir })
}

/// Decodes a LICH byte into `out[4]` (rfct, fct, opt, dir); false when the parity is wrong (`out` untouched).
#[no_mangle]
pub unsafe extern "C" fn onair_nxdn_lich_decode(v: u32, out: *mut i32) -> bool {
    let v = v as u8;
    match lich::decode(v) {
        Some(l) => {
            let o = wr(out, 4);
            if o.len() == 4 {
                o[0] = l.rfct;
                o[1] = l.fct;
                o[2] = l.opt;
                o[3] = l.dir;
            }
            true
        }
        None => false,
    }
}

/// CRC-6 over `n` bits (one per byte).
#[no_mangle]
pub unsafe extern "C" fn onair_nxdn_crc6(bits: *const u8, n: usize) -> u32 {
    crc::crc6(rd(bits, n))
}
/// CRC-12 over `n` bits.
#[no_mangle]
pub unsafe extern "C" fn onair_nxdn_crc12(bits: *const u8, n: usize) -> u32 {
    crc::crc12(rd(bits, n))
}
/// CRC-16 over `n` bits.
#[no_mangle]
pub unsafe extern "C" fn onair_nxdn_crc16(bits: *const u8, n: usize) -> u32 {
    crc::crc16(rd(bits, n))
}

/// Information bits of channel `ch` (0 SACCH, 1 FACCH1, 2 CAC): 26, 80, 155; 0 for an unknown channel.
#[no_mangle]
pub extern "C" fn onair_nxdn_info_bits(ch: i32) -> usize {
    Ch::from_i32(ch).map_or(0, chan::info_bits)
}
/// Air bits of channel `ch`: 60, 144, 300; 0 for an unknown channel.
#[no_mangle]
pub extern "C" fn onair_nxdn_air_bits(ch: i32) -> usize {
    Ch::from_i32(ch).map_or(0, chan::air_bits)
}

/// Encodes `info` (info_bits bits) into `air` (air_bits bits); false on a short buffer or unknown channel.
#[no_mangle]
pub unsafe extern "C" fn onair_nxdn_encode(ch: i32, info: *const u8, info_len: usize, air: *mut u8, air_len: usize) -> bool {
    match Ch::from_i32(ch) {
        Some(c) => chan::encode(c, rd(info, info_len), wr(air, air_len)).is_some(),
        None => false,
    }
}

/// Soft decode of channel `ch` from `soft` (air_bits values, positive = 1) into `info`. Returns 1 when the CRC holds (then `*errs` is set, if not
/// null), 0 when it does not, -1 on a short buffer or unknown channel. `info` is written in every case but -1.
#[no_mangle]
pub unsafe extern "C" fn onair_nxdn_decode(
    ch: i32,
    soft: *const f32,
    soft_len: usize,
    info: *mut u8,
    info_len: usize,
    errs: *mut i32,
) -> i32 {
    let Some(c) = Ch::from_i32(ch) else { return -1 };
    match chan::decode(c, rd(soft, soft_len), wr(info, info_len)) {
        Some(d) => {
            if d.ok && !errs.is_null() {
                *errs = d.errs;
            }
            d.ok as i32
        }
        None => -1,
    }
}

fn to_c(m: &Msg) -> OnairNxdnMsg {
    OnairNxdnMsg {
        typ: m.typ,
        call_type: m.call_type,
        option: m.option,
        src: m.src,
        dst: m.dst,
        cipher: m.cipher,
        key_id: m.key_id,
        status: m.status,
        block: m.block,
        blocks: m.blocks,
        location: m.location,
        service: m.service,
        channel: m.channel,
        text: m.text,
        text_len: m.text_len as u32,
    }
}
fn from_c(m: &OnairNxdnMsg) -> Msg {
    Msg {
        typ: m.typ,
        call_type: m.call_type,
        option: m.option,
        src: m.src,
        dst: m.dst,
        cipher: m.cipher,
        key_id: m.key_id,
        status: m.status,
        block: m.block,
        blocks: m.blocks,
        text: m.text,
        text_len: (m.text_len as usize).min(8),
        location: m.location,
        service: m.service,
        channel: m.channel,
    }
}

/// Builds the message into `bits[0..max_bits]` (cleared first); returns the bit count of the message.
#[no_mangle]
pub unsafe extern "C" fn onair_nxdn_build_msg(m: *const OnairNxdnMsg, bits: *mut u8, max_bits: usize) -> usize {
    if m.is_null() {
        return 0;
    }
    msg::build(&from_c(&*m), wr(bits, max_bits))
}

/// Parses `n_bits` bits into `*m` (always written, type included); false when the body is too short for its type.
#[no_mangle]
pub unsafe extern "C" fn onair_nxdn_parse_msg(bits: *const u8, n_bits: usize, m: *mut OnairNxdnMsg) -> bool {
    let mut r = Msg::default();
    let ok = msg::parse(rd(bits, n_bits), &mut r);
    if !m.is_null() {
        *m = to_c(&r);
    }
    ok
}

/// Static name of message type `t`.
#[no_mangle]
pub extern "C" fn onair_nxdn_msg_name(t: i32) -> *const u8 {
    msg::msg_name(t).as_ptr()
}
/// Static name of call type `t`.
#[no_mangle]
pub extern "C" fn onair_nxdn_call_type_name(t: i32) -> *const u8 {
    msg::call_type_name(t).as_ptr()
}

/// Spreads 72 voice bits into `fr[96]` (mbelib `ambe_fr[4][24]`); false on a wrong buffer size.
#[no_mangle]
pub unsafe extern "C" fn onair_nxdn_ambe_frame(bits: *const u8, n: usize, fr: *mut u8, fr_len: usize) -> bool {
    let (b, f) = (rd(bits, n), wr(fr, fr_len));
    match (<&[u8; 72]>::try_from(b), <&mut [u8; 96]>::try_from(f)) {
        (Ok(b), Ok(f)) => {
            msg::ambe_frame(b, f);
            true
        }
        _ => false,
    }
}
