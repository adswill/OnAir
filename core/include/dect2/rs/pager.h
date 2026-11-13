// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_pager (POCSAG/FLEX). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
// Bits are one per byte, first transmitted bit first.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One decoded FLEX page (mirrors the Rust Page struct; sizeof is checked at start-up with onair_pager_page_size).
typedef struct OnairPagerPage {
    uint32_t address;       // capcode
    int32_t mtype;          // PagerMsgType: 0 tone, 1 numeric, 2 alpha, 3 secure, 4 binary
    int32_t fixed;          // bits the BCH code corrected in the words of this page
    uint8_t damaged;        // a word of the message could not be repaired
    uint8_t pad[3];
    uint32_t len;           // used bytes of text
    uint8_t text[320];
} OnairPagerPage;

// ABI version of this module.
uint32_t onair_pager_version(void);
// sizeof(OnairPagerPage) as Rust sees it.
size_t onair_pager_page_size(void);

// ---- BCH(31,21) code word (ITU-R M.584-2)
// 21 data bits (first transmitted bit in bit 20) -> the 32-bit word with check bits and parity.
uint32_t onair_pager_encode_word(uint32_t data21);
// Checks and repairs a word: returns the corrected bits (0 to 2, the parity bit counts) and writes the 21 data bits; -1 when it cannot be repaired.
int32_t onair_pager_correct_word(uint32_t w, uint32_t* data);
// Reverses the low 21 bits.
uint32_t onair_pager_rev21(uint32_t v);

// ---- POCSAG
// Classifies a received code word: 0 failed, 1 idle, 2 address (a = top 18 address bits, b = function bits), 3 data (a = 20 bits).
// bits = corrected bits (address and data words), fixed = 1 when the word needed a repair (any non-failed word).
int32_t onair_pager_pocsag_classify(uint32_t raw, uint32_t* a, uint32_t* b, int32_t* bits, int32_t* fixed);
// The message bits read as 7-bit characters look like real text.
bool onair_pager_looks_alpha(const uint8_t* bits, size_t n);
// Bits -> 7-bit characters (printable, CR, LF, tab); buffer must hold n / 7 bytes. Returns the length.
size_t onair_pager_alpha_from_bits(const uint8_t* bits, size_t n, uint8_t* out, size_t cap);
// Bits -> 4-bit digits (flex: FLEX character set, filler 12 dropped); buffer must hold n / 4 bytes. Returns the length.
size_t onair_pager_numeric_from_bits(const uint8_t* bits, size_t n, uint32_t flex, uint8_t* out, size_t cap);

// ---- FLEX
// Speed table: index of the entry closest to a sync code (within max_distance bits), of a PagerSpeed; -1 when none.
int32_t onair_pager_flex_mode_for_code(uint32_t code, int32_t max_distance);
int32_t onair_pager_flex_mode_for_speed(int32_t speed);
int32_t onair_pager_flex_mode_count(void);
// Fields of an entry; false for an index out of range.
bool onair_pager_flex_mode_info(int32_t idx, uint16_t* code, int32_t* baud, int32_t* levels, int32_t* speed);
// Frame information word with its checksum / check of one (writes cycle and frame on success).
uint32_t onair_pager_flex_fiw_data(int32_t cycle, int32_t frame);
bool onair_pager_flex_fiw_check(uint32_t f, int32_t* cycle, int32_t* frame);
// Decodes one phase (2816 bits) into at most cap pages; stat = int[4] {ok, fixed, failed, biwOk} that is added to. Returns the number of pages.
size_t onair_pager_flex_decode_phase(const uint8_t* bits, OnairPagerPage* out, size_t cap, int32_t* stat);

#ifdef __cplusplus
}
#endif
