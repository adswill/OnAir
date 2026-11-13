// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_p25 (P25 Phase 1 frame layer). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
// Bits are one per byte, first bit on the air first; hexbits are 6-bit values.
// Names are static NUL-terminated strings. Functions with fixed-size arrays take exactly that many elements.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OnairP25Tsbk { uint8_t last, prot; int32_t opcode, mfid; uint64_t args; } OnairP25Tsbk;
typedef struct OnairP25Lc { int32_t lco, mfid; uint8_t prot; int32_t svc_opts, tg, src, dst; uint64_t raw; } OnairP25Lc;
typedef struct OnairP25Crypto { uint8_t mi[9]; int32_t alg_id, key_id, mfid, tg; } OnairP25Crypto;

// ABI version of this module (2).
uint32_t onair_p25_version(void);
// sizeof of the three structs above in this order: the C++ side checks them at start-up.
void onair_p25_struct_sizes(size_t out[3]);

const uint8_t* onair_p25_duid_name(int32_t d);
int32_t onair_p25_frame_len(int32_t d);                  // dibits, status symbols included; 0 for an unknown data unit

// NID: NAC + DUID -> 64 bits (BCH (63,16,23) + parity). Decode: true within 10 errors (nac, duid set); errs gets the distance either way.
void onair_p25_nid_encode(int32_t nac, int32_t duid, uint8_t* bits64);
bool onair_p25_nid_decode(const uint8_t* bits64, int32_t* nac, int32_t* duid, int32_t* errs);

// Reed-Solomon over GF(64), block length n <= 63, one hexbit per byte. encode fills sym[k..n]; decode corrects in place (errs: corrections made).
bool onair_p25_rs_encode(uint8_t* sym, int32_t n, int32_t k);
bool onair_p25_rs_decode(uint8_t* sym, int32_t n, int32_t k, int32_t* errs);

uint16_t onair_p25_hamming10_encode(int32_t hex);
int32_t onair_p25_hamming10_decode(uint32_t cw, int32_t* errs);
uint32_t onair_p25_golay24_encode(int32_t d12);
int32_t onair_p25_golay24_decode(uint32_t cw, int32_t* errs);   // -1 beyond three errors
uint32_t onair_p25_golay18_encode(int32_t hex);
int32_t onair_p25_golay18_decode(uint32_t cw, int32_t* errs);   // -1 beyond three errors

uint16_t onair_p25_crc_ccitt(const uint8_t* bits, size_t n);
void onair_p25_trellis_encode(const uint8_t* bits96, uint8_t* dibits98);
bool onair_p25_trellis_decode(const float* lv98, uint8_t* bits96, float* metric);   // bits always written; true: a path ends in the zero state

void onair_p25_tsbk_bits(const OnairP25Tsbk* t, uint8_t* bits96);
bool onair_p25_tsbk_parse(const uint8_t* bits96, OnairP25Tsbk* t);                  // *t always written; true: CRC matches
const uint8_t* onair_p25_opcode_name(int32_t op);
void onair_p25_lc_hexbits(const OnairP25Lc* l, uint8_t* hex12);
bool onair_p25_lc_parse(const uint8_t* hex12, OnairP25Lc* l);
const uint8_t* onair_p25_alg_name(int32_t a);
void onair_p25_es_hexbits(const OnairP25Crypto* c, uint8_t* hex16);
void onair_p25_es_parse(const uint8_t* hex16, OnairP25Crypto* c);                   // sets mi, alg_id, key_id only
void onair_p25_hdu_hexbits(const OnairP25Crypto* c, uint8_t* hex20);
void onair_p25_hdu_parse(const uint8_t* hex20, OnairP25Crypto* c);

// IMBE frame: 72 dibits <-> the 8 x 23 code vector matrix (184 bytes, row major) <-> 144 bits
void onair_p25_imbe_deinterleave(const uint8_t* dibits72, uint8_t* fr184);
void onair_p25_imbe_interleave(const uint8_t* fr184, uint8_t* dibits72);
void onair_p25_imbe_flatten(const uint8_t* fr184, uint8_t* bits144);
void onair_p25_imbe_unflatten(const uint8_t* bits144, uint8_t* fr184);

size_t onair_p25_channel_text(int32_t ch, uint8_t* out, size_t cap);                // "id-channel", 0 when cap is too small

#ifdef __cplusplus
}
#endif
