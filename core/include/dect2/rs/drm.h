// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_drm (DRM signalling and data service parsing). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against
// ffi.rs. Bits are one per byte (0 or 1), most significant first. Names are written into a
// caller buffer and the length returned. The record layouts are checked at start-up with onair_drm_struct_sizes().
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OnairDrmFacService { uint32_t id; int32_t short_id; uint8_t audio_ca, data_ca; int32_t language; uint8_t data; int32_t descriptor; } OnairDrmFacService;
typedef struct OnairDrmFacInfo {
    uint8_t enhancement, mode_e; int32_t identity, occupancy, interleaver, msc_mode, sdc_mode, num_services_code, reconfig, toggle, n_svc; OnairDrmFacService svc[2];
} OnairDrmFacInfo;
typedef struct OnairDrmSdcStream { int32_t len_a, len_b; } OnairDrmSdcStream;
typedef struct OnairDrmSdcMux { uint8_t present; int32_t prot_a, prot_b, n_streams; OnairDrmSdcStream stream[4]; } OnairDrmSdcMux;
typedef struct OnairDrmSdcAudio {
    uint8_t present, text, enhancement; int32_t short_id, stream_id, coding, sbr, mode, rate_code, mps; uint32_t n_config; uint8_t config[128];
} OnairDrmSdcAudio;
typedef struct OnairDrmSdcLabel { uint8_t present; int32_t short_id, text_control; uint32_t n_text; uint8_t text[128]; } OnairDrmSdcLabel;
typedef struct OnairDrmSdcLang { uint8_t present, n_language, n_country; int32_t short_id; uint8_t language[3], country[2]; } OnairDrmSdcLang;
typedef struct OnairDrmSdcTime { uint8_t present, has_offset; int32_t mjd, hour, minute, offset_half_hours; } OnairDrmSdcTime;
typedef struct OnairDrmSdcApp {
    uint8_t present, packet, data_unit; int32_t short_id, stream_id, domain, packet_id, packet_len; uint32_t n_app; uint8_t app_data[128];
} OnairDrmSdcApp;
typedef struct OnairDrmSdcInfo {
    int32_t entities; uint32_t n_other; OnairDrmSdcMux mux, mux_next; OnairDrmSdcTime time;
    OnairDrmSdcAudio audio[4]; OnairDrmSdcLabel label[4]; OnairDrmSdcLang lang[4]; OnairDrmSdcApp app[4];
} OnairDrmSdcInfo;
typedef struct OnairDrmAacInfo { int32_t num_frames, hp_bytes; uint8_t header_ok; uint32_t frame_len[16]; uint8_t crc[16]; } OnairDrmAacInfo;
typedef struct OnairDrmTextSeg { uint8_t crc_ok, toggle, first, last, command; int32_t cmd, segnum; uint32_t n; } OnairDrmTextSeg;

// ABI version of this module (1).
uint32_t onair_drm_version(void);
// sizeof of the structs above in the order FacInfo, SdcInfo, AacInfo, TextSeg.
void onair_drm_struct_sizes(size_t out[4]);

uint32_t onair_drm_crc8_bits(const uint8_t* b, size_t n);     // FAC CRC over unpacked bits
uint32_t onair_drm_crc16_bits(const uint8_t* b, size_t n);    // SDC CRC over unpacked bits

bool onair_drm_fac_parse(const uint8_t* b, size_t n, uint32_t mode_e, OnairDrmFacInfo* out);   // 72 / 116 bits; false: wrong CRC or RM flag
// Parses the SDC data field into info (in and out: entities refine what is there); entity types that are not interpreted go to other (info->n_other of them).
int32_t onair_drm_sdc_parse(const uint8_t* d, size_t n, OnairDrmSdcInfo* info, uint8_t* other, size_t ocap);
bool onair_drm_sdc_check_bits(const uint8_t* b, size_t n, size_t field_bytes, int32_t* afs);
int32_t onair_drm_aac_num_frames(uint32_t mode_e, int32_t rate_hz);
int32_t onair_drm_aac_header_bytes(int32_t n);
bool onair_drm_aac_super_frame_parse(const uint8_t* lf, size_t n, int32_t len_a, int32_t frames_n, OnairDrmAacInfo* info, uint8_t* frames, size_t cap);   // frames: the bytes of the logical frame

size_t onair_drm_language_name(int32_t c, uint8_t* out, size_t cap);           // 24 bytes are enough
size_t onair_drm_programme_type_name(int32_t c, uint8_t* out, size_t cap);     // 24
size_t onair_drm_msc_mode_name(uint32_t mode_e, int32_t m, uint8_t* out, size_t cap);   // 16
size_t onair_drm_occupancy_name(uint32_t mode_e, int32_t o, uint8_t* out, size_t cap);  // 16
size_t onair_drm_app_name(int32_t domain, int32_t app, uint8_t* out, size_t cap);   // 32
int32_t onair_drm_fac_audio_services(int32_t c);
int32_t onair_drm_fac_data_services(int32_t c);
void onair_drm_mjd_to_date(int32_t mjd, int32_t* y, int32_t* m, int32_t* d);
int32_t onair_drm_date_to_mjd(int32_t y, int32_t m, int32_t d);

bool onair_drm_group_payload(const uint8_t* d, size_t n, size_t* off, size_t* len);
size_t onair_drm_display_text(const uint8_t* b, size_t n, uint8_t* out, size_t cap);   // 2 * min(n, 16384) bytes
bool onair_drm_looks_text(const uint8_t* b, size_t n);
int32_t onair_drm_text_segment_need(uint32_t b0);
void onair_drm_text_segment_parse(const uint8_t* seg, size_t n, int32_t need, OnairDrmTextSeg* out);

#ifdef __cplusplus
}
#endif
