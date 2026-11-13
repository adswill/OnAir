// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_same (SAME / EAS). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
// Strings returned as `const uint8_t*` are static, NUL-terminated ("" when unknown).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The checks the C++ side runs at start-up: sizeof(OnairSameMsg) and sizeof(OnairSameLoc) as Rust sees them.
typedef struct OnairSameLoc {
    uint8_t ok, region_known, has_pos;   // ok: the code is six digits
    int32_t pca, state, county;
    double lat, lon;
} OnairSameLoc;

// The parsed header (the offsets refer to the parsed text). Like the C++ parser it keeps what was read before the first error.
typedef struct OnairSameMsg {
    uint8_t valid, eom, org_set, event_set, call_set;
    uint32_t nloc;   // location codes read: code k is the six digits at byte 13 + 7 * k
    int32_t purge_min, issue_day, issue_hour, issue_min;
    size_t call_start, call_len;
    const uint8_t *err, *org_name, *event_name, *event_kind;
} OnairSameMsg;

// ABI version of this module (1).
uint32_t onair_same_version(void);
void onair_same_struct_sizes(size_t* msg, size_t* loc);

const uint8_t* onair_same_originator_name(const uint8_t* code, size_t n);   // 47 CFR 11.31 (d)
const uint8_t* onair_same_event_name(const uint8_t* code, size_t n);        // 47 CFR 11.31 (e)
const uint8_t* onair_same_event_kind(const uint8_t* code, size_t n);        // from the table, else the last letter
const uint8_t* onair_same_pca_name(int32_t digit);
const uint8_t* onair_same_state_name(int32_t fips, int32_t country);        // country 0 US, 1 Canada
const uint8_t* onair_same_state_abbr(int32_t fips, int32_t country);
bool onair_same_state_centre(int32_t fips, int32_t country, double* lat, double* lon);
bool onair_same_valid_char(uint32_t c);
// The message cut to the format: writes the start offset, returns the length (0: no "ZCZC" or "NNNN").
size_t onair_same_trim_message(const uint8_t* b, size_t n, size_t* start);

// One location code in words (county_name: the user's table entry or ""); returns the text length or SIZE_MAX when cap is too small.
size_t onair_same_locate(const uint8_t* code, size_t n, int32_t country, const uint8_t* county_name, size_t county_len, OnairSameLoc* info, uint8_t* out, size_t cap);
void onair_same_parse(const uint8_t* b, size_t n, OnairSameMsg* m);

int64_t onair_same_issue_epoch(int32_t year, int32_t day, int32_t hour, int32_t minute);
size_t onair_same_issue_text(int32_t year, int32_t day, int32_t hour, int32_t minute, uint8_t* out, size_t cap);
size_t onair_same_purge_text(int32_t minutes, uint8_t* out, size_t cap);

// County CSV text to records (u32 LE key = state * 1000 + county, u32 LE name length, name): out >= 2 * n + 16 and scratch >= n are always enough.
size_t onair_same_read_county_csv(const uint8_t* t, size_t n, uint8_t* out, size_t cap, uint8_t* scratch, size_t scap);

#ifdef __cplusplus
}
#endif
