// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_kris (Kris modes: LDPC family and the K = 7 convolutional code). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
// Bits are one per byte; LLRs are f32, positive = bit 0 more likely.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ABI version of this module (1).
uint32_t onair_kris_version(void);

// Information bits k of LDPC code (n, rate), n = 1920 / 2304 / 7680, rate index 0..6 = 1/4 1/3 1/2 2/3 3/4 5/6 9/10. -1 for another n or rate, or when n x rate is not an
// integer (n = 2304 at 9/10).
int32_t onair_kris_ldpc_k(int32_t n, int32_t rate);
// Systematic encoder: info = k bits, codeword = n bits (information first, then parity). 0, or -1 for a bad code or a null pointer.
int32_t onair_kris_ldpc_encode(int32_t n, int32_t rate, const uint8_t* info, uint8_t* codeword);
// Layered normalised min-sum decoder (alpha 0.8): llr = n values, info_bits = k bits (written also on failure). Returns the iterations used (0 = the input
// already satisfied every check), or -1 when the checks never passed (or a bad argument).
int32_t onair_kris_ldpc_decode(int32_t n, int32_t rate, const float* llr, uint8_t* info_bits, int32_t max_iter);

// K = 7 convolutional encoder. kind 0 = rate 1/2 (0171, 0133), 1 = rate 1/4 (0171, 0133, 0165, 0117). terminate != 0 appends six zero tail bits.
// out needs (nbits + 6 * (terminate != 0)) * 2 (kind 0) or * 4 (kind 1) bytes. Returns the number of output bits, or -1.
int32_t onair_kris_conv_encode(int32_t kind, const uint8_t* bits, int32_t nbits, int32_t terminate, uint8_t* out);
// Soft Viterbi decoder. llr = nsym trellis steps of 2 (kind 0) or 4 (kind 1) values. terminated != 0: the block ended in state 0, the six tail bits are dropped.
// out_bits needs nsym (minus 6 when terminated) bytes. Returns the number of decoded bits, or -1.
int32_t onair_kris_viterbi(int32_t kind, const float* llr, int32_t nsym, int32_t terminated, uint8_t* out_bits);

// KAVB FEC groups, erasure code over GF(256): g data cells and p parity cells of cell_len bytes (g + p <= 256); any g of the g + p cells restore the group.
// encode: data = g * cell_len bytes, parity (p * cell_len bytes) is written. 0, or -1 for bad arguments.
int32_t onair_kris_erasure_encode(int32_t g, int32_t p, int32_t cell_len, const uint8_t* data, uint8_t* parity);
// recover: cells = (g + p) * cell_len bytes, present = g + p flags (non-zero = received). Missing data cells are rebuilt in place (parity cells are not touched).
// 0 when every data cell is available afterwards, -1 when fewer than g cells are present or the arguments are bad.
int32_t onair_kris_erasure_recover(int32_t g, int32_t p, int32_t cell_len, uint8_t* cells, const uint8_t* present);

#ifdef __cplusplus
}
#endif
