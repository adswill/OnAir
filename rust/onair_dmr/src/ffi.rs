// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_dmr` (core/include/dect2/rs/dmr.h). Every export is `onair_dmr_*`; pointers come as (pointer, length) pairs or point at fixed-size bit /
//! byte arrays documented in the header; the caller owns all buffers. Functions that write text return its length, or `usize::MAX` when the buffer is too
//! small. Names are static NUL-terminated strings. A null pointer where a buffer is needed makes the function fail (false / -1 / 0), nothing is written.
#![allow(clippy::missing_safety_doc)]

use crate::abi::*;
use crate::burst;
use crate::codes::{self, Hamming};
use crate::data;
use crate::lc::{self, Csbk};
use crate::product;
use crate::text::{self, Buf};

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
pub extern "C" fn onair_dmr_version() -> u32 {
    2
}

/// sizeof of the five shared structs (full LC, CSBK, data header, data block, talker alias): the C++ side checks them at start-up.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_struct_sizes(out: *mut usize) {
    if let Some(o) = am::<usize, 5>(out) {
        *o = [
            core::mem::size_of::<OnairDmrFullLc>(),
            core::mem::size_of::<OnairDmrCsbk>(),
            core::mem::size_of::<OnairDmrDataHeader>(),
            core::mem::size_of::<OnairDmrDataBlock>(),
            core::mem::size_of::<OnairDmrTa>(),
        ];
    }
}

// ---------------------------------------------------------------------------------------------------- short codes

/// Golay (20,8) parity of the slot type information byte.
#[no_mangle]
pub extern "C" fn onair_dmr_golay2008_parity(info8: u32) -> u32 {
    codes::golay2008_parity(info8)
}
/// Golay (20,8) decode within 3 errors: the error count and `*info8`, or -1.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_golay2008_decode(word20: u32, info8: *mut u32) -> i32 {
    match codes::golay2008_decode(word20) {
        Some((e, i)) => {
            if !info8.is_null() {
                *info8 = i;
            }
            e
        }
        None => -1,
    }
}
/// QR (16,7,6) parity of the EMB information bits.
#[no_mangle]
pub extern "C" fn onair_dmr_qr1676_parity(info7: u32) -> u32 {
    codes::qr1676_parity(info7)
}
/// QR (16,7,6) decode within 2 errors.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_qr1676_decode(word16: u32, info7: *mut u32) -> i32 {
    match codes::qr1676_decode(word16) {
        Some((e, i)) => {
            if !info7.is_null() {
                *info7 = i;
            }
            e
        }
        None => -1,
    }
}
/// Hamming (7,4) parity of the TACT information bits.
#[no_mangle]
pub extern "C" fn onair_dmr_hamming743_parity(info4: u32) -> u32 {
    codes::hamming743_parity(info4)
}
/// Hamming (7,4) decode with single error correction.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_hamming743_decode(word7: u32, info4: *mut u32) -> i32 {
    match codes::hamming743_decode(word7) {
        Some((e, i)) => {
            if !info4.is_null() {
                *info4 = i;
            }
            e
        }
        None => -1,
    }
}
/// Check bits of the product code Hamming `kind` (0 (13,9), 1 (15,11), 2 (16,11), 3 (17,12)).
#[no_mangle]
pub extern "C" fn onair_dmr_hamming_parity(kind: i32, info: u32) -> u32 {
    codes::hamming_parity(Hamming::from_i32(kind), info)
}
/// Corrects one bit of `*word`: 0 clean, 1 corrected, -1 not correctable.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_hamming_correct(kind: i32, word: *mut u32) -> i32 {
    if word.is_null() {
        return -1;
    }
    codes::hamming_correct(Hamming::from_i32(kind), &mut *word)
}
/// Information bits of Hamming `kind`.
#[no_mangle]
pub extern "C" fn onair_dmr_hamming_info_bits(kind: i32) -> i32 {
    Hamming::from_i32(kind).info_bits() as i32
}
/// Check bits of Hamming `kind`.
#[no_mangle]
pub extern "C" fn onair_dmr_hamming_parity_bits(kind: i32) -> i32 {
    Hamming::from_i32(kind).parity_bits() as i32
}
/// Reed-Solomon (12,9) parity (3 bytes) of 9 message bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_rs129_parity(msg: *const u8, parity: *mut u8) {
    if let (Some(m), Some(p)) = (ar::<u8, 9>(msg), am::<u8, 3>(parity)) {
        *p = codes::rs129_parity(m);
    }
}
/// Corrects one symbol of a (12,9) word: 0 clean, 1 corrected, -1 failed.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_rs129_correct(word: *mut u8) -> i32 {
    match am::<u8, 12>(word) {
        Some(w) => codes::rs129_correct(w),
        None => -1,
    }
}
/// CRC-CCITT with final inversion over `n` bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_crc_ccitt(d: *const u8, n: usize) -> u16 {
    codes::crc_ccitt(rd(d, n))
}
/// CRC-8 of the short LC over 28 bits.
#[no_mangle]
pub extern "C" fn onair_dmr_crc8_short(bits28: u32) -> u8 {
    codes::crc8_short(bits28)
}
/// CRC-9 of a confirmed block before the mask: `n` data octets and the serial number.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_crc9(d: *const u8, n: usize, dbsn: u32) -> u16 {
    codes::crc9(rd(d, n), dbsn)
}
/// Message CRC-32 over `n` bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_crc32_msg(d: *const u8, n: usize) -> u32 {
    codes::crc32_msg(rd(d, n))
}
/// Embedded LC checksum of 9 bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_checksum5(lc: *const u8) -> u8 {
    ar::<u8, 9>(lc).map_or(0, codes::checksum5)
}

// ---------------------------------------------------------------------------------------------------- product codes

/// BPTC (196,96) encode: 96 info bits -> 196 bits.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_bptc196_encode(info96: *const u8, tx196: *mut u8) {
    if let (Some(i), Some(t)) = (ar::<u8, 96>(info96), am::<u8, 196>(tx196)) {
        *t = product::bptc196_encode(i);
    }
}
/// BPTC (196,96) decode: bits corrected or -1.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_bptc196_decode(tx196: *const u8, info96: *mut u8) -> i32 {
    match (ar::<u8, 196>(tx196), am::<u8, 96>(info96)) {
        (Some(t), Some(i)) => product::bptc196_decode(t, i),
        _ => -1,
    }
}
/// Embedded LC encode: 9 bytes -> four fragments of 32 bits (128 bytes).
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_emb_lc_encode(lc: *const u8, frag: *mut u8) {
    if let (Some(l), Some(f)) = (ar::<u8, 9>(lc), am::<[u8; 32], 4>(frag as *mut [u8; 32])) {
        product::emb_lc_encode(l, f);
    }
}
/// Embedded LC decode from four fragments (128 bytes): bits corrected, -1 uncorrectable, -2 checksum wrong.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_emb_lc_decode(frag: *const u8, lc: *mut u8) -> i32 {
    match (ar::<[u8; 32], 4>(frag as *const [u8; 32]), am::<u8, 9>(lc)) {
        (Some(f), Some(l)) => product::emb_lc_decode(f, l),
        _ => -1,
    }
}
/// Short LC encode: 28 bits -> four pieces of 17 bits (68 bytes).
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_short_lc_encode(lc28: u32, piece: *mut u8) {
    if let Some(p) = am::<[u8; 17], 4>(piece as *mut [u8; 17]) {
        product::short_lc_encode(lc28, p);
    }
}
/// Short LC decode from four pieces (68 bytes): bits corrected, -1 uncorrectable, -2 CRC mismatch.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_short_lc_decode(piece: *const u8, lc28: *mut u32) -> i32 {
    match (ar::<[u8; 17], 4>(piece as *const [u8; 17]), lc28.is_null()) {
        (Some(p), false) => product::short_lc_decode(p, &mut *lc28),
        _ => -1,
    }
}
/// CACH encode: TACT fields and 17 payload bits -> 24 bits.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_cach_encode(at: i32, tc: i32, lcss: i32, payload17: u32, out24: *mut u8) {
    if let Some(o) = am::<u8, 24>(out24) {
        *o = product::cach_encode(at, tc, lcss, payload17);
    }
}
/// CACH decode: TACT errors corrected or -1; `*payload17` is always set, the TACT fields only on success.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_cach_decode(in24: *const u8, at: *mut i32, tc: *mut i32, lcss: *mut i32, payload17: *mut u32) -> i32 {
    let Some(i) = ar::<u8, 24>(in24) else { return -1 };
    let (c, p) = product::cach_decode(i);
    if !payload17.is_null() {
        *payload17 = p;
    }
    match c {
        Some(c) => {
            if !at.is_null() {
                *at = c.at;
            }
            if !tc.is_null() {
                *tc = c.tc;
            }
            if !lcss.is_null() {
                *lcss = c.lcss;
            }
            c.errors
        }
        None => -1,
    }
}
/// Rate 3/4 trellis encode: 18 bytes -> 196 bits.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_trellis34_encode(data: *const u8, tx196: *mut u8) {
    if let (Some(d), Some(t)) = (ar::<u8, 18>(data), am::<u8, 196>(tx196)) {
        *t = product::trellis34_encode(d);
    }
}
/// Rate 3/4 trellis decode of 98 soft symbols into 18 bytes: dibits differing from the best path, or -1.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_trellis34_decode(symbols: *const f32, data: *mut u8) -> i32 {
    match (ar::<f32, 98>(symbols), am::<u8, 18>(data)) {
        (Some(s), Some(d)) => product::trellis34_decode(s, d),
        _ => -1,
    }
}
/// Rate 1 encode: 24 bytes -> 196 bits.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_rate1_encode(data: *const u8, tx196: *mut u8) {
    if let (Some(d), Some(t)) = (ar::<u8, 24>(data), am::<u8, 196>(tx196)) {
        *t = product::rate1_encode(d);
    }
}
/// Rate 1 decode: 24 bytes; returns true when the four pad bits are zero.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_rate1_decode(tx196: *const u8, data: *mut u8) -> bool {
    match (ar::<u8, 196>(tx196), am::<u8, 24>(data)) {
        (Some(t), Some(d)) => product::rate1_decode(t, d),
        _ => false,
    }
}

// ---------------------------------------------------------------------------------------------------- bursts

/// Name of sync pattern `s`.
#[no_mangle]
pub extern "C" fn onair_dmr_sync_name(s: i32) -> *const u8 {
    burst::sync_name(s).as_ptr()
}
/// The 48-bit value of sync pattern `s` (0 when out of range).
#[no_mangle]
pub extern "C" fn onair_dmr_sync_word(s: i32) -> u64 {
    if (0..burst::SYNC_COUNT as i32).contains(&s) {
        burst::SYNC_WORDS[s as usize]
    } else {
        0
    }
}
/// 1 or 2 for the direct mode patterns, 0 otherwise.
#[no_mangle]
pub extern "C" fn onair_dmr_sync_direct_slot(s: i32) -> i32 {
    burst::sync_direct_slot(s)
}
/// Pattern of a family and kind.
#[no_mangle]
pub extern "C" fn onair_dmr_sync_of(family: i32, voice: u32) -> i32 {
    let voice = voice != 0;
    burst::sync_of(family, voice)
}
/// The 24 symbols (+3 / -3) of pattern `s` as signed bytes; false when `s` is out of range.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_sync_symbols(s: i32, out24: *mut i8) -> bool {
    match am::<i8, 24>(out24) {
        Some(o) => burst::sync_symbols(s, o),
        None => false,
    }
}
/// Best sync pattern for a 48-bit centre field (bits one per byte): its index or -1 when none is within `max_diff`; `*diff` (if not null) gets the best distance.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_match_sync(centre48: *const u8, max_diff: i32, diff: *mut i32) -> i32 {
    match ar::<u8, 48>(centre48) {
        Some(c) => {
            let (b, d) = burst::match_sync(c, max_diff);
            if !diff.is_null() {
                *diff = d;
            }
            b
        }
        None => -1,
    }
}
/// Data type name.
#[no_mangle]
pub extern "C" fn onair_dmr_data_type_name(dt: i32) -> *const u8 {
    burst::data_type_name(dt).as_ptr()
}
/// Short data type name.
#[no_mangle]
pub extern "C" fn onair_dmr_data_type_short(dt: i32) -> *const u8 {
    burst::data_type_short(dt).as_ptr()
}
/// Assembles a 264-bit data burst; false when `sync` is not -1 or a pattern index.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_make_data_burst(cc: i32, dt: i32, payload196: *const u8, sync: i32, out264: *mut u8) -> bool {
    match (ar::<u8, 196>(payload196), am::<u8, 264>(out264)) {
        (Some(p), Some(o)) => burst::make_data_burst(cc, dt, p, sync, o),
        _ => false,
    }
}
/// Assembles a 264-bit voice burst from VS(215)..VS(0) and (without a sync) the 32 embedded bits.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_make_voice_burst(
    vs216: *const u8,
    sync: i32,
    cc: i32,
    pi: i32,
    lcss: i32,
    emb32: *const u8,
    out264: *mut u8,
) -> bool {
    match (ar::<u8, 216>(vs216), ar::<u8, 32>(emb32), am::<u8, 264>(out264)) {
        (Some(v), Some(e), Some(o)) => burst::make_voice_burst(v, sync, cc, pi, lcss, e, o),
        _ => false,
    }
}
/// Slot type of a burst of `n` bits: true and the fields, or false when Golay (20,8) cannot correct it.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_burst_slot_type(b: *const u8, n: usize, cc: *mut i32, dt: *mut i32, errors: *mut i32) -> bool {
    match burst::burst_slot_type(rd(b, n)) {
        Some((c, d, e)) => {
            if !cc.is_null() {
                *cc = c;
            }
            if !dt.is_null() {
                *dt = d;
            }
            if !errors.is_null() {
                *errors = e;
            }
            true
        }
        None => false,
    }
}
/// EMB of a burst of `n` bits: `emb32` always receives the 32 embedded bits; true and the fields when QR (16,7,6) corrects the EMB.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_burst_emb(
    b: *const u8,
    n: usize,
    emb32: *mut u8,
    cc: *mut i32,
    pi: *mut i32,
    lcss: *mut i32,
    errors: *mut i32,
) -> bool {
    let mut tmp = [0u8; 32];
    let r = burst::burst_emb(rd(b, n), &mut tmp);
    if let Some(e) = am::<u8, 32>(emb32) {
        *e = tmp;
    }
    match r {
        Some((c, p, l, e)) => {
            if !cc.is_null() {
                *cc = c;
            }
            if !pi.is_null() {
                *pi = p;
            }
            if !lcss.is_null() {
                *lcss = l;
            }
            if !errors.is_null() {
                *errors = e;
            }
            true
        }
        None => false,
    }
}
/// 4FSK symbols of `n` bits (n / 2 values) into `out` (capacity `cap`); returns the count written.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_bits_to_symbols(bits: *const u8, n: usize, out: *mut i8, cap: usize) -> usize {
    if out.is_null() {
        return 0;
    }
    burst::bits_to_symbols(rd(bits, n), core::slice::from_raw_parts_mut(out, cap))
}

// ---------------------------------------------------------------------------------------------------- link control

/// Packs a full LC into 9 bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_pack_full_lc(lc: *const OnairDmrFullLc, out9: *mut u8) {
    if let (false, Some(o)) = (lc.is_null(), am::<u8, 9>(out9)) {
        *o = lc::pack_full_lc(&lc_from_c(&*lc));
    }
}
/// Reads a full LC from 9 bytes (the `raw` copy included).
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_parse_full_lc(in9: *const u8, lc: *mut OnairDmrFullLc) {
    if let (Some(i), false) = (ar::<u8, 9>(in9), lc.is_null()) {
        *lc = lc_to_c(&lc::parse_full_lc(i));
    }
}
/// Name of a full LC opcode.
#[no_mangle]
pub extern "C" fn onair_dmr_flco_name(f: i32) -> *const u8 {
    lc::flco_name(f).as_ptr()
}
/// Service options in words into `out`; returns the length or SIZE_MAX.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_service_options_text(svc: u32, out: *mut u8, cap: usize) -> usize {
    let svc = svc as u8;
    let mut b = Buf::new(wr(out, cap));
    lc::service_options_text(svc, &mut b);
    b.result()
}
/// Header / terminator burst info bits (96) of a nine byte LC.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_lc_burst_info(lc: *const u8, terminator: u32, info96: *mut u8) {
    let terminator = terminator != 0;
    if let (Some(l), Some(i)) = (ar::<u8, 9>(lc), am::<u8, 96>(info96)) {
        lc::lc_burst_info(l, terminator, i);
    }
}
/// Decodes a header / terminator burst: 0 clean, 1 one octet corrected, -1 failed; the nine bytes are always written.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_lc_burst_decode(info96: *const u8, terminator: u32, lc: *mut u8) -> i32 {
    let terminator = terminator != 0;
    match (ar::<u8, 96>(info96), am::<u8, 9>(lc)) {
        (Some(i), Some(l)) => lc::lc_burst_decode(i, terminator, l),
        _ => -1,
    }
}
/// 96 bits of ten octets plus their CRC-CCITT XOR `mask`.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_crc_block_info(data10: *const u8, mask: u32, info96: *mut u8) {
    let mask = mask as u16;
    if let (Some(d), Some(i)) = (ar::<u8, 10>(data10), am::<u8, 96>(info96)) {
        lc::crc_block_info(d, mask, i);
    }
}
/// Checks the CRC of a 96 bit block; the ten octets are always written.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_crc_block_check(info96: *const u8, mask: u32, data10: *mut u8) -> bool {
    let mask = mask as u16;
    match (ar::<u8, 96>(info96), am::<u8, 10>(data10)) {
        (Some(i), Some(d)) => lc::crc_block_check(i, mask, d),
        _ => false,
    }
}
/// The 96 bits of a CSBK with CRC.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_csbk_info(c: *const OnairDmrCsbk, info96: *mut u8) {
    if let (false, Some(i)) = (c.is_null(), am::<u8, 96>(info96)) {
        let c = &*c;
        lc::csbk_info(&Csbk { lb: c.lb != 0, pf: c.pf != 0, opcode: c.opcode, fid: c.fid, data: c.data }, i);
    }
}
/// Parses a CSBK (CRC checked); `*c` is written only when true.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_csbk_parse(info96: *const u8, c: *mut OnairDmrCsbk) -> bool {
    match (ar::<u8, 96>(info96), c.is_null()) {
        (Some(i), false) => match lc::csbk_parse(i) {
            Some(r) => {
                *c = OnairDmrCsbk { lb: r.lb as u8, pf: r.pf as u8, opcode: r.opcode, fid: r.fid, data: r.data };
                true
            }
            None => false,
        },
        _ => false,
    }
}
/// CSBK opcode name.
#[no_mangle]
pub extern "C" fn onair_dmr_csbk_name(op: i32, fid: i32) -> *const u8 {
    lc::csbk_name(op, fid).as_ptr()
}

// ---------------------------------------------------------------------------------------------------- data

/// Name of a data packet format.
#[no_mangle]
pub extern "C" fn onair_dmr_dpf_name(d: i32) -> *const u8 {
    data::dpf_name(d).as_ptr()
}
/// Name of a service access point.
#[no_mangle]
pub extern "C" fn onair_dmr_sap_name(s: i32) -> *const u8 {
    data::sap_name(s).as_ptr()
}
/// Packs a data header into ten octets.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_pack_data_header(h: *const OnairDmrDataHeader, out10: *mut u8) {
    if let (false, Some(o)) = (h.is_null(), am::<u8, 10>(out10)) {
        *o = data::pack_data_header(&dh_from_c(&*h));
    }
}
/// Parses ten octets; `*h` is always written (only the format set for an unknown one); false for an unknown format.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_parse_data_header(in10: *const u8, h: *mut OnairDmrDataHeader) -> bool {
    match (ar::<u8, 10>(in10), h.is_null()) {
        (Some(i), false) => {
            let (ok, r) = data::parse_data_header(i);
            *h = dh_to_c(&r);
            ok
        }
        _ => false,
    }
}
/// Octets of a data block of type `dt`.
#[no_mangle]
pub extern "C" fn onair_dmr_data_block_bytes(dt: i32, confirmed: u32) -> i32 {
    let confirmed = confirmed != 0;
    data::data_block_bytes(dt, confirmed)
}
/// Encodes a data block (`n` user bytes) into 196 payload bits.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_data_block_encode(dt: i32, user: *const u8, n: usize, confirmed: u32, dbsn: u32, out196: *mut u8) {
    let confirmed = confirmed != 0;
    if let Some(o) = am::<u8, 196>(out196) {
        *o = data::data_block_encode(dt, rd(user, n), confirmed, dbsn);
    }
}
/// Splits a decoded block (`n` octets, at least the block size); false for a type that is not a data type or too few octets.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_data_block_parse(
    dt: i32,
    oct: *const u8,
    n: usize,
    confirmed: u32,
    out: *mut OnairDmrDataBlock,
) -> bool {
    let confirmed = confirmed != 0;
    if out.is_null() {
        return false;
    }
    match data::data_block_parse(dt, rd(oct, n), confirmed) {
        Some(b) => {
            *out = OnairDmrDataBlock { bytes: b.bytes, len: b.len as u32, dbsn: b.dbsn, crc_ok: b.crc_ok as u8 };
            true
        }
        None => false,
    }
}

// ---------------------------------------------------------------------------------------------------- text

/// Name of data format `dd`.
#[no_mangle]
pub extern "C" fn onair_dmr_dd_name(dd: i32) -> *const u8 {
    text::dd_name(dd).as_ptr()
}
/// Short data / talker alias text of `n` bytes; the text needs at most 2 * n + 80 bytes. Returns its length or SIZE_MAX.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_text_from_bytes(p: *const u8, n: usize, format: i32, ta: u32, out: *mut u8, cap: usize) -> usize {
    let ta = ta != 0;
    let mut b = Buf::new(wr(out, cap));
    text::text_from_bytes(rd(p, n), format, ta, &mut b);
    b.result()
}
/// Adds a talker alias header or block LC to the alias.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_ta_add(t: *mut OnairDmrTa, lc: *const OnairDmrFullLc) {
    if t.is_null() || lc.is_null() {
        return;
    }
    let mut a = ta_from_c(&*t);
    a.add(&lc_from_c(&*lc));
    *t = ta_to_c(&a);
}
/// True when the header and all needed blocks are in.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_ta_complete(t: *const OnairDmrTa) -> bool {
    !t.is_null() && ta_from_c(&*t).complete()
}
/// The alias text (empty until complete) into `out`; returns its length or SIZE_MAX.
#[no_mangle]
pub unsafe extern "C" fn onair_dmr_ta_text(t: *const OnairDmrTa, out: *mut u8, cap: usize) -> usize {
    let mut b = Buf::new(wr(out, cap));
    if !t.is_null() {
        ta_from_c(&*t).text(&mut b);
    }
    b.result()
}
/// Number of PDUs (header + blocks) a talker alias of `length` characters in `format` needs.
#[no_mangle]
pub extern "C" fn onair_dmr_alias_pieces(format: i32, length: i32) -> i32 {
    text::alias_pieces(format, length)
}
