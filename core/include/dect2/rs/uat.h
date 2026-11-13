// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_uat (UAT 978 MHz message layer, RTCA DO-282B). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
// Flags are 0 / 1 bytes; the structs are plain C (no bit fields).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One decoded ADS-B / TIS-B downlink message (the fields of dump978's struct uat_adsb_mdb). Enums are the numbers of the C enums.
typedef struct OnairUatAdsb {
    uint8_t has_sv, has_ms, has_auxsv, position_valid, ns_vel_valid, ew_vel_valid, speed_valid, dimensions_valid, position_offset, utc_coupled;
    uint8_t has_cdti, has_acas, acas_ra_active, ident_active, atc_services;
    uint8_t mdb_type, address_qualifier, nic, altitude_type, airground_state, track_type, vert_rate_source, tisb_site_id;
    uint8_t emitter_category, callsign_type, emergency_status, uat_version, sil, transmit_mso, nac_p, nac_v, nic_baro, heading_type, sec_altitude_type;
    uint8_t callsign[9];                       // NUL terminated
    uint32_t address;
    int32_t altitude, sec_altitude;
    double lat, lon, length, width;
    int16_t ns_vel, ew_vel, vert_rate;
    uint16_t track, speed;
} OnairUatAdsb;
// FIS-B APDU header; data_off is the offset of the product data in the uplink application data.
typedef struct OnairUatFisb {
    uint8_t a_flag, g_flag, p_flag, s_flag, monthday_valid, seconds_valid, month, day, hours, minutes, seconds;
    uint16_t product_id, length, data_off;
} OnairUatFisb;
// One information frame; data_off is the offset of its payload in the application data (frame bytes 8..431).
typedef struct OnairUatInfo { uint8_t is_fisb, kind; uint16_t length, data_off; OnairUatFisb fisb; } OnairUatInfo;
// A decoded uplink frame. tail_*: the slot after the last counted frame, in which the reference stored length and type when its scan stopped there.
typedef struct OnairUatUplink {
    uint8_t position_valid, utc_coupled, app_data_valid, slot_id, tisb_site_id, num_info_frames, tail_valid, tail_kind;
    uint16_t tail_length;
    double lat, lon;
    OnairUatInfo info[70];
} OnairUatUplink;

// ABI version of this module (2).
uint32_t onair_uat_version(void);
// sizeof of OnairUatAdsb and OnairUatUplink: the C++ side checks them at start-up.
void onair_uat_struct_sizes(size_t out[2]);

// Decode a downlink payload (n bytes, bytes past n read as 0; 31 are used) / an uplink frame (n bytes, 432 are used). A NULL frame reads as all zero.
void onair_uat_decode_adsb(const uint8_t* frame, size_t n, OnairUatAdsb* out);
void onair_uat_decode_uplink(const uint8_t* frame, size_t n, OnairUatUplink* out);

#ifdef __cplusplus
}
#endif
