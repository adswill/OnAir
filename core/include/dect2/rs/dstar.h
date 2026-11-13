// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_dstar (D-STAR DV frame layer). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
// Bits are one per byte, bytes go on the air least significant bit first.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OnairDstarFix { double lat, lon, alt_m, speed_kmh; int32_t course; uint8_t alt_valid, vel_valid, aprs; } OnairDstarFix;
// An unpacked header: NUL terminated callsign fields (printable characters, trailing spaces removed).
typedef struct OnairDstarHeader { uint8_t flag[3]; uint8_t rpt2[9], rpt1[9], ur[9], my[9], sfx[9]; } OnairDstarHeader;

// ABI version of this module (2).
uint32_t onair_dstar_version(void);
// sizeof of the two structs above in this order: the C++ side checks them at start-up.
void onair_dstar_struct_sizes(size_t out[2]);

// CRC-CCITT as D-STAR uses it (reflected 0x8408, start 0xFFFF, inverted), sent low byte first.
uint16_t onair_dstar_crc(const uint8_t* p, size_t n);

// Header: pack flag[3] and five fields (pointer + length, cut / space padded to 8, 8, 8, 8, 4) into 41 bytes with the CRC; unpack (false: CRC mismatch, *h written).
void onair_dstar_header_pack(const uint8_t* flag, const uint8_t* rpt2, size_t rpt2_n, const uint8_t* rpt1, size_t rpt1_n, const uint8_t* ur, size_t ur_n,
                             const uint8_t* my, size_t my_n, const uint8_t* sfx, size_t sfx_n, uint8_t* out41);
bool onair_dstar_header_unpack(const uint8_t* in41, OnairDstarHeader* h);
size_t onair_dstar_flag_text(const uint8_t* flag3, uint8_t* out, size_t cap);   // at most 96 bytes

// Header on the air: 41 bytes <-> 660 bits (convolutional code, interleaver, scrambler); decode takes soft values (positive = 1) and returns the corrected channel bits.
void onair_dstar_header_encode(const uint8_t* hdr41, uint8_t* bits660);
int32_t onair_dstar_header_decode(const float* soft660, uint8_t* hdr41);
void onair_dstar_scramble_bits(uint8_t* bits, size_t n);
void onair_dstar_interleave(const uint8_t* in660, uint8_t* out660);
void onair_dstar_deinterleave(const float* in660, float* out660);
void onair_dstar_conv_encode(const uint8_t* in, size_t n_in, uint8_t* out);   // 2 * n_in bits
int32_t onair_dstar_viterbi(const float* soft, size_t n_out, uint8_t* out);   // n_out <= 1024; -1 otherwise
void onair_dstar_bytes_to_bits(const uint8_t* b, size_t n_bytes, uint8_t* bits);
void onair_dstar_bits_to_bytes(const uint8_t* bits, size_t n_bytes, uint8_t* b);
void onair_dstar_ambe_frame(const uint8_t* bits72, uint8_t* fr96);   // mbelib's ambe_fr[4][24]

// GPS text. nmea_checksum: two hex digits of the XOR of body. Parsers return 0: rejected early (*f untouched), 1: rejected after the reference had written to the fix
// (*f holds that state), 2: parsed. parse_dprs needs call and comment buffers of n bytes each.
void onair_dstar_nmea_checksum(const uint8_t* body, size_t n, uint8_t* out2);
uint16_t onair_dstar_dprs_crc(const uint8_t* s, size_t n);
int32_t onair_dstar_parse_nmea(const uint8_t* line, size_t n, OnairDstarFix* f);
int32_t onair_dstar_parse_dprs(const uint8_t* line, size_t n, OnairDstarFix* f, uint8_t* call, size_t* call_len, uint8_t* comment, size_t* comment_len);

#ifdef __cplusplus
}
#endif
