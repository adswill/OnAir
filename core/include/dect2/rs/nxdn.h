// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_nxdn (NXDN frame layer). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
// Bits are one per byte. Names are static NUL-terminated strings.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A layer 3 message (text: text_len bytes, no terminator). The C++ side checks sizeof against onair_nxdn_struct_size() at start-up.
typedef struct OnairNxdnMsg {
    int32_t type, call_type, option, src, dst, cipher, key_id, status, block, blocks;
    uint32_t location;
    int32_t service, channel;
    uint8_t text[8];
    uint32_t text_len;
} OnairNxdnMsg;

// ABI version of this module (2).
uint32_t onair_nxdn_version(void);
size_t onair_nxdn_struct_size(void);

// The PN9 scrambler bits (x^9 + x^5 + 1, seed 0xE4), n of them.
void onair_nxdn_pn9(uint8_t* out, size_t n);
// LICH byte (8 bits MSB first, even parity last) and its decoding into out[4] = rfct, fct, opt, dir; false: parity wrong.
uint8_t onair_nxdn_lich_encode(int32_t rfct, int32_t fct, int32_t opt, int32_t dir);
bool onair_nxdn_lich_decode(uint32_t v, int32_t* out);
// CRC-6 / 12 / 16 over n bits.
uint32_t onair_nxdn_crc6(const uint8_t* bits, size_t n);
uint32_t onair_nxdn_crc12(const uint8_t* bits, size_t n);
uint32_t onair_nxdn_crc16(const uint8_t* bits, size_t n);

// Channels: 0 SACCH, 1 FACCH1, 2 CAC. Information bits before the CRC (26, 80, 155) and bits on air (60, 144, 300); 0 for an unknown channel.
size_t onair_nxdn_info_bits(int32_t ch);
size_t onair_nxdn_air_bits(int32_t ch);
// info (info_bits) -> air (air_bits): CRC, convolutional code, puncturing, interleaving. False on a short buffer.
bool onair_nxdn_encode(int32_t ch, const uint8_t* info, size_t info_len, uint8_t* air, size_t air_len);
// Soft decode (positive = 1). 1: CRC holds (*errs, when not null, gets the channel bit errors), 0: CRC fails, -1: short buffer. info is written unless -1.
int32_t onair_nxdn_decode(int32_t ch, const float* soft, size_t soft_len, uint8_t* info, size_t info_len, int32_t* errs);

// Layer 3: build into bits[0..max_bits] (cleared first), returns the message's bit count; parse n_bits bits into *m (always written), false when too short.
size_t onair_nxdn_build_msg(const OnairNxdnMsg* m, uint8_t* bits, size_t max_bits);
bool onair_nxdn_parse_msg(const uint8_t* bits, size_t n_bits, OnairNxdnMsg* m);
const uint8_t* onair_nxdn_msg_name(int32_t t);
const uint8_t* onair_nxdn_call_type_name(int32_t t);

// 72 voice bits into mbelib's ambe_fr[4][24] (96 bytes). False on wrong sizes.
bool onair_nxdn_ambe_frame(const uint8_t* bits, size_t n, uint8_t* fr, size_t fr_len);

#ifdef __cplusplus
}
#endif
