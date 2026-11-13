// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_p25` (core/include/dect2/rs/p25.h). Every export is `onair_p25_*`; pointers come as (pointer, length) pairs or point at fixed-size bit /
//! byte arrays documented in the header; the caller owns all buffers. A null pointer where a buffer is needed makes the function fail (false / -1 / 0),
//! nothing is written. Names are static NUL-terminated strings.
#![allow(clippy::missing_safety_doc)]

use crate::msg::{self, Crypto, Lc, Tsbk};
use crate::{golay, imbe, nid, rs, trellis};

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
pub extern "C" fn onair_p25_version() -> u32 {
    2
}

/// A TSBK block.
#[repr(C)]
pub struct OnairP25Tsbk {
    pub last: u8,
    pub prot: u8,
    pub opcode: i32,
    pub mfid: i32,
    pub args: u64,
}
/// A link control word.
#[repr(C)]
pub struct OnairP25Lc {
    pub lco: i32,
    pub mfid: i32,
    pub prot: u8,
    pub svc_opts: i32,
    pub tg: i32,
    pub src: i32,
    pub dst: i32,
    pub raw: u64,
}
/// Encryption sync / header fields.
#[repr(C)]
pub struct OnairP25Crypto {
    pub mi: [u8; 9],
    pub alg_id: i32,
    pub key_id: i32,
    pub mfid: i32,
    pub tg: i32,
}

/// sizeof of the three shared structs (TSBK, link control, crypto): the C++ side checks them at start-up.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_struct_sizes(out: *mut usize) {
    if let Some(o) = am::<usize, 3>(out) {
        *o = [core::mem::size_of::<OnairP25Tsbk>(), core::mem::size_of::<OnairP25Lc>(), core::mem::size_of::<OnairP25Crypto>()];
    }
}

/// Data unit name.
#[no_mangle]
pub extern "C" fn onair_p25_duid_name(d: i32) -> *const u8 {
    msg::duid_name(d).as_ptr()
}
/// Frame length in dibits of a data unit (0 unknown).
#[no_mangle]
pub extern "C" fn onair_p25_frame_len(d: i32) -> i32 {
    msg::frame_len(d)
}

/// NID encode: NAC and DUID -> 64 bits.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_nid_encode(nac: i32, duid: i32, bits: *mut u8) {
    if let Some(b) = am::<u8, 64>(bits) {
        nid::encode(nac, duid, b);
    }
}
/// NID decode of 64 bits: true when within 10 errors (`*nac`, `*duid` set); `*errs` (if not null) gets the distance to the nearest codeword either way.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_nid_decode(bits: *const u8, nac: *mut i32, duid: *mut i32, errs: *mut i32) -> bool {
    let Some(b) = ar::<u8, 64>(bits) else { return false };
    let r = nid::decode(b);
    if !errs.is_null() {
        *errs = r.errs;
    }
    if r.ok {
        if !nac.is_null() {
            *nac = r.nac;
        }
        if !duid.is_null() {
            *duid = r.duid;
        }
    }
    r.ok
}

/// Reed-Solomon parity: fills `sym[k..n]` from `sym[0..k]`; false for n > 63 or k > n.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_rs_encode(sym: *mut u8, n: i32, k: i32) -> bool {
    if n < 0 || k < 0 {
        return false;
    }
    rs::encode(wr(sym, n as usize), n as usize, k as usize)
}
/// Reed-Solomon decode in place: true when the result is a codeword; `*errs` (if not null) gets the corrections made (0 when decoding stopped early).
#[no_mangle]
pub unsafe extern "C" fn onair_p25_rs_decode(sym: *mut u8, n: i32, k: i32, errs: *mut i32) -> bool {
    if !errs.is_null() {
        *errs = 0;
    }
    if n < 0 || k < 0 {
        return false;
    }
    let (ok, e) = rs::decode(wr(sym, n as usize), n as usize, k as usize);
    if !errs.is_null() {
        *errs = e;
    }
    ok
}

/// Hamming (10,6,3) codeword of a hexbit.
#[no_mangle]
pub extern "C" fn onair_p25_hamming10_encode(hex: i32) -> u16 {
    golay::hamming10_encode(hex)
}
/// Hamming (10,6,3) decode: the hexbit; `*errs` (if not null) gets the distance.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_hamming10_decode(cw: u32, errs: *mut i32) -> i32 {
    let cw = cw as u16;
    let (h, e) = golay::hamming10_decode(cw);
    if !errs.is_null() {
        *errs = e;
    }
    h
}
/// Golay (24,12,8) codeword of 12 data bits.
#[no_mangle]
pub extern "C" fn onair_p25_golay24_encode(d12: i32) -> u32 {
    golay::golay24_encode(d12)
}
/// Golay (24,12,8) decode: the data, or -1 beyond three errors; `*errs` gets the distance.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_golay24_decode(cw: u32, errs: *mut i32) -> i32 {
    let (d, e) = golay::golay24_decode(cw);
    if !errs.is_null() {
        *errs = e;
    }
    d
}
/// Golay (18,6,8) codeword of a hexbit.
#[no_mangle]
pub extern "C" fn onair_p25_golay18_encode(hex: i32) -> u32 {
    golay::golay18_encode(hex)
}
/// Golay (18,6,8) decode: the hexbit, or -1 beyond three errors; `*errs` gets the distance.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_golay18_decode(cw: u32, errs: *mut i32) -> i32 {
    let (h, e) = golay::golay18_decode(cw);
    if !errs.is_null() {
        *errs = e;
    }
    h
}

/// CRC-CCITT (complemented) over `n` bits.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_crc_ccitt(bits: *const u8, n: usize) -> u16 {
    trellis::crc_ccitt(rd(bits, n))
}
/// Rate 1/2 trellis encode: 96 bits -> 98 dibits.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_trellis_encode(bits: *const u8, dibits: *mut u8) {
    if let (Some(b), Some(d)) = (ar::<u8, 96>(bits), am::<u8, 98>(dibits)) {
        trellis::encode(b, d);
    }
}
/// Rate 1/2 trellis decode of 98 levels: true when a path ends in the zero state; the bits are written either way, `*metric` (if not null) gets the path metric.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_trellis_decode(lv: *const f32, bits: *mut u8, metric: *mut f32) -> bool {
    let (Some(l), Some(b)) = (ar::<f32, 98>(lv), am::<u8, 96>(bits)) else { return false };
    let (m, ok) = trellis::decode(l, b);
    if !metric.is_null() {
        *metric = m;
    }
    ok
}

/// TSBK to 96 bits with CRC.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_tsbk_bits(t: *const OnairP25Tsbk, bits: *mut u8) {
    if let (false, Some(b)) = (t.is_null(), am::<u8, 96>(bits)) {
        let t = &*t;
        msg::tsbk_bits(&Tsbk { last: t.last != 0, prot: t.prot != 0, opcode: t.opcode, mfid: t.mfid, args: t.args }, b);
    }
}
/// Reads a TSBK from 96 bits (always written); returns true when the CRC matches.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_tsbk_parse(bits: *const u8, t: *mut OnairP25Tsbk) -> bool {
    match (ar::<u8, 96>(bits), t.is_null()) {
        (Some(b), false) => {
            let (r, ok) = msg::tsbk_parse(b);
            *t = OnairP25Tsbk { last: r.last as u8, prot: r.prot as u8, opcode: r.opcode, mfid: r.mfid, args: r.args };
            ok
        }
        _ => false,
    }
}
/// TSBK opcode name.
#[no_mangle]
pub extern "C" fn onair_p25_opcode_name(op: i32) -> *const u8 {
    msg::opcode_name(op).as_ptr()
}

/// Link control word to 12 hexbits.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_lc_hexbits(l: *const OnairP25Lc, hex: *mut u8) {
    if let (false, Some(h)) = (l.is_null(), am::<u8, 12>(hex)) {
        let l = &*l;
        msg::lc_hexbits(
            &Lc { lco: l.lco, mfid: l.mfid, prot: l.prot != 0, svc_opts: l.svc_opts, tg: l.tg, src: l.src, dst: l.dst, raw: l.raw },
            h,
        );
    }
}
/// Reads a link control word from 12 hexbits (always written); returns true.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_lc_parse(hex: *const u8, l: *mut OnairP25Lc) -> bool {
    match (ar::<u8, 12>(hex), l.is_null()) {
        (Some(h), false) => {
            let r = msg::lc_parse(h);
            *l = OnairP25Lc {
                lco: r.lco,
                mfid: r.mfid,
                prot: r.prot as u8,
                svc_opts: r.svc_opts,
                tg: r.tg,
                src: r.src,
                dst: r.dst,
                raw: r.raw,
            };
            true
        }
        _ => false,
    }
}

fn crypto_from_c(c: &OnairP25Crypto) -> Crypto {
    Crypto { mi: c.mi, alg_id: c.alg_id, key_id: c.key_id, mfid: c.mfid, tg: c.tg }
}
fn crypto_to_c(c: &Crypto) -> OnairP25Crypto {
    OnairP25Crypto { mi: c.mi, alg_id: c.alg_id, key_id: c.key_id, mfid: c.mfid, tg: c.tg }
}
/// Algorithm name.
#[no_mangle]
pub extern "C" fn onair_p25_alg_name(a: i32) -> *const u8 {
    msg::alg_name(a).as_ptr()
}
/// Encryption sync to 16 hexbits.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_es_hexbits(c: *const OnairP25Crypto, hex: *mut u8) {
    if let (false, Some(h)) = (c.is_null(), am::<u8, 16>(hex)) {
        msg::es_hexbits(&crypto_from_c(&*c), h);
    }
}
/// Reads an encryption sync into `*c` (message indicator, algorithm, key id; the other fields are kept).
#[no_mangle]
pub unsafe extern "C" fn onair_p25_es_parse(hex: *const u8, c: *mut OnairP25Crypto) {
    if let (Some(h), false) = (ar::<u8, 16>(hex), c.is_null()) {
        let mut r = crypto_from_c(&*c);
        msg::es_parse(h, &mut r);
        *c = crypto_to_c(&r);
    }
}
/// Header word to 20 hexbits.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_hdu_hexbits(c: *const OnairP25Crypto, hex: *mut u8) {
    if let (false, Some(h)) = (c.is_null(), am::<u8, 20>(hex)) {
        msg::hdu_hexbits(&crypto_from_c(&*c), h);
    }
}
/// Reads a header word into `*c`.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_hdu_parse(hex: *const u8, c: *mut OnairP25Crypto) {
    if let (Some(h), false) = (ar::<u8, 20>(hex), c.is_null()) {
        let mut r = crypto_from_c(&*c);
        msg::hdu_parse(h, &mut r);
        *c = crypto_to_c(&r);
    }
}

/// 72 dibits into the 8 x 23 code vector matrix (184 bytes).
#[no_mangle]
pub unsafe extern "C" fn onair_p25_imbe_deinterleave(dibits: *const u8, fr: *mut u8) {
    if let (Some(d), Some(f)) = (ar::<u8, 72>(dibits), am::<u8, 184>(fr)) {
        imbe::deinterleave(d, f);
    }
}
/// The matrix back into 72 dibits.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_imbe_interleave(fr: *const u8, dibits: *mut u8) {
    if let (Some(f), Some(d)) = (ar::<u8, 184>(fr), am::<u8, 72>(dibits)) {
        imbe::interleave(f, d);
    }
}
/// The matrix as 144 bits, c0 first.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_imbe_flatten(fr: *const u8, bits: *mut u8) {
    if let (Some(f), Some(b)) = (ar::<u8, 184>(fr), am::<u8, 144>(bits)) {
        imbe::flatten(f, b);
    }
}
/// 144 bits into the matrix.
#[no_mangle]
pub unsafe extern "C" fn onair_p25_imbe_unflatten(bits: *const u8, fr: *mut u8) {
    if let (Some(b), Some(f)) = (ar::<u8, 144>(bits), am::<u8, 184>(fr)) {
        imbe::unflatten(b, f);
    }
}

/// Channel number as "id-channel" text into `out`; returns the length (0 when `cap` is too small).
#[no_mangle]
pub unsafe extern "C" fn onair_p25_channel_text(ch: i32, out: *mut u8, cap: usize) -> usize {
    msg::channel_text(ch, wr(out, cap))
}
