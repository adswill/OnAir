// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_fec` (core/include/dect2/rs/fec.h). Every export is `onair_fec_*`; pointers come as (pointer, length) pairs and the caller owns all buffers.
//! Failures never crash: a null pointer or a wrong length gives the documented error value (-2 / false / `usize::MAX`).
#![allow(clippy::missing_safety_doc)]

use crate::bch::BchCtx;
use crate::ldpc::{self, LdpcOut};
use crate::rs;

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_fec_version() -> u32 {
    1
}

/// Input slice (empty for a null pointer or length 0).
unsafe fn rd<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(p, n)
    }
}

/// Output slice (empty for a null pointer or length 0).
unsafe fn wr<'a>(p: *mut u8, n: usize) -> &'a mut [u8] {
    if p.is_null() || n == 0 {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(p, n)
    }
}

/// Reed-Solomon codes: 0 = DVB-T (204,188), 1 = ATSC (207,187), 2 = DAB (120,110).
const RS_DVBT: i32 = 0;
const RS_ATSC: i32 = 1;
const RS_DAB: i32 = 2;

/// Systematic RS encoder of code `code`: `data` is n - R bytes (188 / 187 / 110), `out` gets the n byte codeword. False for a bad code, null pointer or length.
#[no_mangle]
pub unsafe extern "C" fn onair_fec_rs_encode(code: i32, data: *const u8, n_data: usize, out: *mut u8, n_out: usize) -> bool {
    if data.is_null() || out.is_null() {
        return false;
    }
    let (d, o) = (rd(data, n_data), wr(out, n_out));
    match code {
        RS_DVBT => rs::DVBT.encode(d, o),
        RS_ATSC => rs::ATSC.encode(d, o),
        RS_DAB => rs::DAB.encode(d, o),
        _ => false,
    }
}

/// RS decoder of code `code` over the `n` byte word `r`, corrected in place. Returns the corrected byte count (0 = clean), -1 uncorrectable, -2 bad code /
/// null pointer / length not equal to the code's n (204 / 207 / 120). The DAB variant leaves the word untouched on -1; the others may leave it partly corrected.
#[no_mangle]
pub unsafe extern "C" fn onair_fec_rs_decode(code: i32, r: *mut u8, n: usize) -> i32 {
    if r.is_null() {
        return -2;
    }
    let w = wr(r, n);
    match code {
        RS_DVBT if n == 204 => rs::DVBT.decode(w, false),
        RS_ATSC if n == 207 => rs::ATSC.decode(w, false),
        RS_DAB if n == 120 => rs::DAB.decode(w, true),
        _ => -2,
    }
}

/// Size in bytes of the BCH context the caller has to provide (8-byte aligned).
#[no_mangle]
pub extern "C" fn onair_fec_bch_ctx_size() -> usize {
    BchCtx::size()
}

/// Initialises a BCH context of `ctx_len` bytes for a short (16200 bit) or normal (64800 bit) frame with t errors (1..12). False for a null / small context or bad t.
#[no_mangle]
pub unsafe extern "C" fn onair_fec_bch_init(ctx: *mut u8, ctx_len: usize, short_frame: u32, t: i32) -> bool {
    let short_frame = short_frame != 0;
    if ctx.is_null() || ctx_len < BchCtx::size() || (ctx as usize) % core::mem::align_of::<BchCtx>() != 0 || t < 1 {
        return false;
    }
    (*(ctx as *mut BchCtx)).init(short_frame, t as u32)
}

/// Parity bits of an initialised context (m * t), 0 for a null pointer.
#[no_mangle]
pub unsafe extern "C" fn onair_fec_bch_parity_bits(ctx: *const u8) -> usize {
    if ctx.is_null() {
        return 0;
    }
    (*(ctx as *const BchCtx)).parity_bits()
}

/// BCH encoder: the message is `bits[..k]` (one bit per byte), the parity is written to `bits[k..k + parity_bits]`. False when the buffer of `n` bytes is too short.
#[no_mangle]
pub unsafe extern "C" fn onair_fec_bch_encode(ctx: *const u8, bits: *mut u8, n: usize, k: usize) -> bool {
    if ctx.is_null() || bits.is_null() {
        return false;
    }
    (*(ctx as *const BchCtx)).encode(wr(bits, n), k)
}

/// BCH decoder over `n` bits (one per byte), corrected in place. Returns the number of corrected bits, -1 uncorrectable, -2 for a null pointer.
#[no_mangle]
pub unsafe extern "C" fn onair_fec_bch_decode(ctx: *const u8, bits: *mut u8, n: usize) -> i32 {
    if ctx.is_null() || (bits.is_null() && n != 0) {
        return -2;
    }
    (*(ctx as *const BchCtx)).decode(wr(bits, n))
}

/// 64-state K = 7 Viterbi range decode (DVB-T / ISDB-T): `soft` is 2 * steps signed bytes, `bits` receives `steps` input bits, `dec` is `steps` words of scratch,
/// `*metric` the final path metric. False when a pointer is null while `steps` > 0 (or `steps` is too large to address).
#[no_mangle]
pub unsafe extern "C" fn onair_fec_viterbi_range(soft: *const i8, steps: usize, bits: *mut u8, dec: *mut u64, metric: *mut i64) -> bool {
    if steps == 0 {
        if !metric.is_null() {
            *metric = 0;
        }
        return true;
    }
    if soft.is_null() || bits.is_null() || dec.is_null() || steps > usize::MAX / 16 {
        return false;
    }
    let s = core::slice::from_raw_parts(soft, 2 * steps);
    let b = core::slice::from_raw_parts_mut(bits, steps);
    let d = core::slice::from_raw_parts_mut(dec, steps);
    match crate::viterbi::range(s, steps, b, d) {
        Some(m) => {
            if !metric.is_null() {
                *metric = m;
            }
            true
        }
        None => false,
    }
}

/// Output arrays of `onair_fec_ldpc_build`: pointer and length (in `i32`) of each, sized from `onair_fec_ldpc_dims`.
#[repr(C)]
pub struct OnairFecLdpcOut {
    /// `m + 1` offsets into `chk_var`.
    pub chk_start: *mut i32,
    /// Length of `chk_start`.
    pub n_chk_start: usize,
    /// Variables of every check.
    pub chk_var: *mut i32,
    /// Length of `chk_var`.
    pub n_chk_var: usize,
    /// Parity index of every information edge.
    pub enc_p: *mut i32,
    /// Length of `enc_p`.
    pub n_enc_p: usize,
    /// Information bit of every information edge.
    pub enc_d: *mut i32,
    /// Length of `enc_d`.
    pub n_enc_d: usize,
    /// `q + 1` offsets into the layer arrays.
    pub layer_start: *mut i32,
    /// Length of `layer_start`.
    pub n_layer_start: usize,
    /// Group of every layer connection.
    pub layer_group: *mut i32,
    /// Length of `layer_group`.
    pub n_layer_group: usize,
    /// Shift of every layer connection.
    pub layer_shift: *mut i32,
    /// Length of `layer_shift`.
    pub n_layer_shift: usize,
}

unsafe fn arr<'a>(p: *mut i32, n: usize) -> &'a mut [i32] {
    if p.is_null() || n == 0 {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(p, n)
    }
}

/// Array sizes for an LDPC code with `k` information bits, `n` bits and `n_addrs` table addresses: out[0..5] = m, q, addresses, information edges, length of `chk_var`.
/// False (and nothing written) for bad dimensions.
#[no_mangle]
pub unsafe extern "C" fn onair_fec_ldpc_dims(k: usize, n: usize, n_addrs: usize, out: *mut usize) -> bool {
    if out.is_null() {
        return false;
    }
    match ldpc::dims(k, n, n_addrs) {
        Some(d) => {
            for (i, v) in [d.m, d.q, d.addrs, d.edges, d.chk_var].iter().enumerate() {
                *out.add(i) = *v;
            }
            true
        }
        None => false,
    }
}

/// Builds the parity-check tables of an IRA LDPC code (see `ldpc.rs`). `row_len[g]` = addresses of group g (`n_rows` groups), `addrs` = all of them in order.
/// False for bad dimensions, a negative address, null pointers or arrays not exactly as long as `onair_fec_ldpc_dims` says.
#[no_mangle]
pub unsafe extern "C" fn onair_fec_ldpc_build(
    k: usize,
    n: usize,
    row_len: *const u32,
    n_rows: usize,
    addrs: *const i32,
    n_addrs: usize,
    out: *mut OnairFecLdpcOut,
) -> bool {
    if out.is_null() || (row_len.is_null() && n_rows != 0) || (addrs.is_null() && n_addrs != 0) {
        return false;
    }
    let o = &*out;
    let rl: &[u32] = if n_rows == 0 { &[] } else { core::slice::from_raw_parts(row_len, n_rows) };
    let ad: &[i32] = if n_addrs == 0 { &[] } else { core::slice::from_raw_parts(addrs, n_addrs) };
    let mut lo = LdpcOut {
        chk_start: arr(o.chk_start, o.n_chk_start),
        chk_var: arr(o.chk_var, o.n_chk_var),
        enc_p: arr(o.enc_p, o.n_enc_p),
        enc_d: arr(o.enc_d, o.n_enc_d),
        layer_start: arr(o.layer_start, o.n_layer_start),
        layer_group: arr(o.layer_group, o.n_layer_group),
        layer_shift: arr(o.layer_shift, o.n_layer_shift),
    };
    ldpc::build(k, n, rl, ad, &mut lo)
}
