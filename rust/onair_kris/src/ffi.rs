// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_kris` (core/include/dect2/rs/kris.h). Every export is `onair_kris_*`; scalars are i32 / u32 (no narrow types), buffers are caller owned,
//! bits are one per byte. A null pointer or a bad size gives -1.
#![allow(clippy::missing_safety_doc)]

use crate::{conv, erasure, ldpc};

/// ABI version of this module's C interface.
#[no_mangle]
pub extern "C" fn onair_kris_version() -> u32 {
    1
}

/// Information bits of the LDPC code (n, rate index 0..6 = 1/4 1/3 1/2 2/3 3/4 5/6 9/10), or -1 (unsupported n or rate, or a non-integer k: n = 2304 at 9/10).
#[no_mangle]
pub extern "C" fn onair_kris_ldpc_k(n: i32, rate: i32) -> i32 {
    if n <= 0 || rate < 0 {
        return -1;
    }
    match ldpc::info_len(n as usize, rate as usize) {
        Some(k) => k as i32,
        None => -1,
    }
}

/// Systematic encode: `info` is k bits, `codeword` receives n bits (the k information bits first). 0, or -1 for a bad code or a null pointer.
#[no_mangle]
pub unsafe extern "C" fn onair_kris_ldpc_encode(n: i32, rate: i32, info: *const u8, codeword: *mut u8) -> i32 {
    if info.is_null() || codeword.is_null() || n <= 0 || rate < 0 {
        return -1;
    }
    let code = match ldpc::code(n as usize, rate as usize) {
        Some(c) => c,
        None => return -1,
    };
    let i = core::slice::from_raw_parts(info, code.k);
    let o = core::slice::from_raw_parts_mut(codeword, code.n);
    if code.encode(i, o) {
        0
    } else {
        -1
    }
}

/// Layered normalised min-sum decoder (alpha 0.8). `llr` is n values (positive = bit 0), `info_bits` receives k bits (also when the decoder gives up).
/// Returns the iterations used (0 = the input already satisfied every check), or -1 when the checks never passed (or a bad argument).
#[no_mangle]
pub unsafe extern "C" fn onair_kris_ldpc_decode(n: i32, rate: i32, llr: *const f32, info_bits: *mut u8, max_iter: i32) -> i32 {
    if llr.is_null() || info_bits.is_null() || n <= 0 || rate < 0 {
        return -1;
    }
    let code = match ldpc::code(n as usize, rate as usize) {
        Some(c) => c,
        None => return -1,
    };
    let l = core::slice::from_raw_parts(llr, code.n);
    let o = core::slice::from_raw_parts_mut(info_bits, code.k);
    code.decode(l, max_iter, o)
}

/// Convolutional encoder, K = 7. kind 0 = rate 1/2, 1 = rate 1/4; `terminate` non-zero appends six zero tail bits. `out` needs
/// (nbits + 6 * (terminate != 0)) * 2 (or 4) bytes. Returns the number of output bits, or -1.
#[no_mangle]
pub unsafe extern "C" fn onair_kris_conv_encode(kind: i32, bits: *const u8, nbits: i32, terminate: i32, out: *mut u8) -> i32 {
    if nbits < 0 || out.is_null() || (bits.is_null() && nbits > 0) {
        return -1;
    }
    let term = terminate != 0;
    let total = match conv::coded_len(kind, nbits as usize, term) {
        Some(t) => t,
        None => return -1,
    };
    let b: &[u8] = if nbits == 0 { &[] } else { core::slice::from_raw_parts(bits, nbits as usize) };
    let o = core::slice::from_raw_parts_mut(out, total);
    match conv::encode_into(kind, b, term, o) {
        Some(w) => w as i32,
        None => -1,
    }
}

/// Soft Viterbi decoder. `llr` holds `nsym` trellis steps of 2 (kind 0) or 4 (kind 1) values (positive = bit 0). `terminated` non-zero: the block ended
/// in the zero state and the six tail bits are dropped. `out_bits` needs nsym (minus 6 when terminated) bytes. Returns the number of decoded bits, or -1.
#[no_mangle]
pub unsafe extern "C" fn onair_kris_viterbi(kind: i32, llr: *const f32, nsym: i32, terminated: i32, out_bits: *mut u8) -> i32 {
    if nsym < 0 || llr.is_null() || out_bits.is_null() {
        return -1;
    }
    let no = match conv::n_out(kind) {
        Some(x) => x,
        None => return -1,
    };
    let term = terminated != 0;
    let nbits = if term { (nsym as usize).saturating_sub(conv::TAIL) } else { nsym as usize };
    let l = core::slice::from_raw_parts(llr, nsym as usize * no);
    let o = core::slice::from_raw_parts_mut(out_bits, nbits);
    match conv::decode_into(kind, l, nsym as usize, term, o) {
        Some(w) => w as i32,
        None => -1,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ffi_roundtrip() {
        assert_eq!(onair_kris_ldpc_k(1920, 2), 960);
        assert_eq!(onair_kris_ldpc_k(2304, 6), -1);
        assert_eq!(onair_kris_ldpc_k(1920, 7), -1);
        assert_eq!(onair_kris_ldpc_k(100, 0), -1);
        let k = onair_kris_ldpc_k(2304, 4) as usize;
        let info: Vec<u8> = (0..k).map(|i| ((i * 7 + i / 3) & 1) as u8).collect();
        let mut cw = vec![0u8; 2304];
        unsafe {
            assert_eq!(onair_kris_ldpc_encode(2304, 4, info.as_ptr(), cw.as_mut_ptr()), 0);
            let mut llr: Vec<f32> = cw.iter().map(|&b| if b == 0 { 2.5 } else { -2.5 }).collect();
            for i in (0..2304).step_by(97) {
                llr[i] = -llr[i] * 0.2;
            }
            let mut out = vec![0u8; k];
            let it = onair_kris_ldpc_decode(2304, 4, llr.as_ptr(), out.as_mut_ptr(), 50);
            assert!(it >= 0);
            assert_eq!(out, info);
            assert_eq!(onair_kris_ldpc_encode(2304, 6, info.as_ptr(), cw.as_mut_ptr()), -1);
            assert_eq!(onair_kris_ldpc_encode(2304, 4, core::ptr::null(), cw.as_mut_ptr()), -1);

            let bits: Vec<u8> = (0..50).map(|i| (i % 3 == 0) as u8).collect();
            let mut coded = vec![0u8; 112];
            assert_eq!(onair_kris_conv_encode(0, bits.as_ptr(), 50, 1, coded.as_mut_ptr()), 112);
            let l: Vec<f32> = coded.iter().map(|&b| if b == 0 { 1.0 } else { -1.0 }).collect();
            let mut d = vec![0u8; 50];
            assert_eq!(onair_kris_viterbi(0, l.as_ptr(), 56, 1, d.as_mut_ptr()), 50);
            assert_eq!(d, bits);
            assert_eq!(onair_kris_viterbi(3, l.as_ptr(), 56, 1, d.as_mut_ptr()), -1);
        }
    }
}

/// KAVB FEC groups (erasure code over GF(256)): parity cells of `g` data cells. `data` is g * cell_len bytes, `parity` receives p * cell_len bytes. 0, or -1 (bad size, null).
#[no_mangle]
pub unsafe extern "C" fn onair_kris_erasure_encode(g: i32, p: i32, cell_len: i32, data: *const u8, parity: *mut u8) -> i32 {
    if g <= 0 || p < 0 || cell_len <= 0 || data.is_null() || (p > 0 && parity.is_null()) {
        return -1;
    }
    let (g, p, l) = (g as usize, p as usize, cell_len as usize);
    let d = core::slice::from_raw_parts(data, g * l);
    let mut empty: [u8; 0] = [];
    let o: &mut [u8] = if p > 0 { core::slice::from_raw_parts_mut(parity, p * l) } else { &mut empty };
    if erasure::encode(g, p, l, d, o) {
        0
    } else {
        -1
    }
}

/// KAVB FEC groups: `cells` is (g + p) * cell_len bytes, `present` g + p flags (non-zero = received). Missing data cells are rebuilt in place. 0 when every data cell is
/// available afterwards, -1 when fewer than g cells are present or the arguments are bad.
#[no_mangle]
pub unsafe extern "C" fn onair_kris_erasure_recover(g: i32, p: i32, cell_len: i32, cells: *mut u8, present: *const u8) -> i32 {
    if g <= 0 || p < 0 || cell_len <= 0 || cells.is_null() || present.is_null() {
        return -1;
    }
    let (g, p, l) = (g as usize, p as usize, cell_len as usize);
    let c = core::slice::from_raw_parts_mut(cells, (g + p) * l);
    let pr = core::slice::from_raw_parts(present, g + p);
    if erasure::recover(g, p, l, c, pr) {
        0
    } else {
        -1
    }
}
