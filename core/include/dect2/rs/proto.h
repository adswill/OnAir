// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust protocol crate rust/onair_proto (written by hand: cbindgen is not available offline). Every function takes (pointer, length) pairs and caller-owned output buffers; functions that write return the
// number of elements written, or SIZE_MAX when the output buffer is too small. Bits are one per byte, in transmission order.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mirror of msg::Msg in rust/onair_proto/src/msg.rs (field order and types must match; onair_ais_msg_size() is checked at start-up).
#define ONAIR_AIS_NAME_CAP 48
#define ONAIR_AIS_CALL_CAP 8
#define ONAIR_AIS_DEST_CAP 24
#define ONAIR_AIS_VENDOR_CAP 4
typedef struct OnairAisMsg {
    uint8_t valid, has_pos, has_rot, off_position, virtual_aid, name_len, callsign_len, destination_len, vendor_len;
    int32_t type, repeat;
    uint32_t mmsi;
    int32_t cls;
    double lat, lon;
    float sog, cog;
    int32_t heading;
    float rot_deg_min;
    int32_t nav_status, second;
    uint32_t imo;
    int32_t ship_type, dim_a, dim_b, dim_c, dim_d, eta_month, eta_day, eta_hour, eta_min;
    float draught_m;
    int32_t epfd, part_no, aid_type, altitude_m, year, month, day, hour, minute, sec, dac, fi;
    uint32_t dest_mmsi;
    size_t text_bits;   // type 14: the text is text_bits bits from bit 40 (fetch it with onair_ais_get_text)
    size_t bits;
    uint8_t name[ONAIR_AIS_NAME_CAP], callsign[ONAIR_AIS_CALL_CAP], destination[ONAIR_AIS_DEST_CAP], vendor[ONAIR_AIS_VENDOR_CAP];
} OnairAisMsg;

uint32_t onair_proto_version(void);
size_t onair_ais_msg_size(void);

uint32_t onair_ais_get_u(const uint8_t* b, size_t n, size_t pos, size_t len);
int32_t onair_ais_get_s(const uint8_t* b, size_t n, size_t pos, size_t len);
size_t onair_ais_get_text(const uint8_t* b, size_t n, size_t pos, size_t len, uint8_t* out, size_t cap);   // out: len / 6 bytes

uint16_t onair_ais_fcs_register(const uint8_t* b, size_t n);
bool onair_ais_fcs_ok(const uint8_t* b, size_t n);
size_t onair_ais_stuff(const uint8_t* b, size_t n, uint8_t* out, size_t cap);        // out: n + n / 5 + 1
size_t onair_ais_destuff(const uint8_t* b, size_t n, uint8_t* out, size_t cap);      // out: n; SIZE_MAX also when six 1s in a row
size_t onair_ais_nrzi_encode(const uint8_t* b, size_t n, int32_t start, uint8_t* out, size_t cap);   // out: n
size_t onair_ais_nrzi_decode(const uint8_t* b, size_t n, int32_t prev, uint8_t* out, size_t cap);    // out: n
void onair_ais_reverse_octets(uint8_t* b, size_t n);
// Good frames one after the other in out (cap >= n), their lengths in lens (lens_cap >= n / 64 + 2); returns their number.
size_t onair_ais_hdlc_frames(const uint8_t* b, size_t n, uint8_t* out, size_t out_cap, size_t* lens, size_t lens_cap, int32_t* bad);

bool onair_ais_decode_message(const uint8_t* b, size_t n, OnairAisMsg* m);
bool onair_ais_mmsi_plausible(uint32_t mmsi);
bool onair_ais_acceptable(int32_t type, uint32_t valid, uint32_t mmsi, uint32_t decoded);
int32_t onair_ais_class_of_mmsi(uint32_t mmsi, int32_t type);
double onair_ais_rate_of_turn(int32_t raw);

size_t onair_ais_armour(const uint8_t* b, size_t n, uint8_t* out, size_t cap, int32_t* fill);   // out: (n + 5) / 6
size_t onair_ais_unarmour(const uint8_t* c, size_t n, int32_t fill, uint8_t* out, size_t cap);  // out: n * 6
uint8_t onair_nmea_checksum(const uint8_t* c, size_t n);
// 0 = rejected (nothing set), 1 = layout wrong (outputs set), 2 = good. The payload characters are s[*chars_start .. *chars_start + *chars_len).
int32_t onair_nmea_parse(const uint8_t* s, size_t n, size_t* chars_start, size_t* chars_len, int32_t* fill, int32_t* frag_count, int32_t* frag_no, uint8_t* channel);

#ifdef __cplusplus
}
#endif
