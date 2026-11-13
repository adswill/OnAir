// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_fec (FEC). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ABI version of this module (1).
uint32_t onair_fec_version(void);

// ---- Reed-Solomon over GF(256), polynomial 0x11D, roots alpha^0 .. alpha^(R-1). code: 0 = DVB-T RS(204,188), 1 = ATSC RS(207,187), 2 = DAB RS(120,110).
// Encoder: `data` is n - R bytes (188 / 187 / 110), `out` receives the n byte codeword. False for a bad code, a null pointer or a wrong length.
bool onair_fec_rs_encode(int32_t code, const uint8_t* data, size_t n_data, uint8_t* out, size_t n_out);
// Decoder: corrects the n byte word in place (n must be 204 / 207 / 120). Returns the number of corrected bytes (0 = clean), -1 when uncorrectable,
// -2 for a bad code, a null pointer or a wrong length. The DAB variant leaves the word untouched on -1; the others may leave it partly corrected.
int32_t onair_fec_rs_decode(int32_t code, uint8_t* r, size_t n);

// ---- DVB-T2 BCH (EN 302 755 section 6.1.1), bits one per byte, first bit = highest power.
// Size of the context the caller allocates (8-byte aligned, e.g. a vector<uint64_t>), then initialises once per code.
size_t onair_fec_bch_ctx_size(void);
// short_frame: 16200 bit frames (GF(2^14)) instead of 64800 (GF(2^16)); t = 1..12 errors. False for a null / small / misaligned context or a bad t.
bool onair_fec_bch_init(uint8_t* ctx, size_t ctx_len, uint32_t short_frame, int32_t t);
// Parity bits of an initialised context (m * t); 0 for a null pointer.
size_t onair_fec_bch_parity_bits(const uint8_t* ctx);
// Encoder: message bits[0..k), parity written to bits[k..k + parity bits). False when the n byte buffer is too short.
bool onair_fec_bch_encode(const uint8_t* ctx, uint8_t* bits, size_t n, size_t k);
// Decoder over n bits, corrected in place: number of corrected bits, -1 uncorrectable, -2 for a null pointer.
int32_t onair_fec_bch_decode(const uint8_t* ctx, uint8_t* bits, size_t n);

// ---- Viterbi, 64-state K = 7 code (171, 133 octal) of DVB-T / ISDB-T. `soft`: 2 * steps signed bytes (positive = bit 0, 0 = punctured); `bits`: steps bytes out
// (the input bit of every step); `dec`: caller scratch of `steps` words (the decisions); *metric: the final path metric. False for a null pointer with steps > 0.
bool onair_fec_viterbi_range(const int8_t* soft, size_t steps, uint8_t* bits, uint64_t* dec, int64_t* metric);

// ---- LDPC parity-check tables (DVB-T2 / DVB-S2 IRA codes, 360 bit groups): the decoders stay in C++, only the table builder is here.
// Address a of group g, information bit c of the group goes to check (a + c * q) mod m, m = n - k, q = m / 360; parity bits form the zig-zag (check j has parity j, j-1).
// Array sizes of a code with k information bits, n bits and n_addrs table addresses: out[0..5] = {m, q, addresses, information edges (= 360 * addresses),
// length of chk_var (= edges + 2 m - 1)}. False (nothing written) when n <= k, m < 360 or the sizes do not fit 31 bits.
bool onair_fec_ldpc_dims(size_t k, size_t n, size_t n_addrs, size_t* out);
// All output arrays are int32 with the lengths below (pointer + length, the length must be exact): chk_start m + 1, chk_var out[4], enc_p and enc_d out[3],
// layer_start q + 1, layer_group and layer_shift n_addrs.
typedef struct OnairFecLdpcOut {
    int32_t* chk_start;   size_t n_chk_start;     // offsets into chk_var, one per check plus the end
    int32_t* chk_var;     size_t n_chk_var;       // variables of every check: information edges (group, bit, address order), then parity j and j-1
    int32_t* enc_p;       size_t n_enc_p;         // parity index of every information edge
    int32_t* enc_d;       size_t n_enc_d;         // information bit of every information edge
    int32_t* layer_start; size_t n_layer_start;   // offsets into layer_group / layer_shift, one per layer a mod q plus the end
    int32_t* layer_group; size_t n_layer_group;   // group of every connection of a layer, in table order
    int32_t* layer_shift; size_t n_layer_shift;   // a / q of the connection
} OnairFecLdpcOut;
// row_len[g] = addresses of group g (n_rows groups), addrs = all addresses in order (n_addrs = the sum of row_len). False for bad dimensions, a negative address,
// a null pointer, row_len not adding up to n_addrs or output arrays of the wrong length.
bool onair_fec_ldpc_build(size_t k, size_t n, const uint32_t* row_len, size_t n_rows, const int32_t* addrs, size_t n_addrs, OnairFecLdpcOut* out);

#ifdef __cplusplus
}
#endif
