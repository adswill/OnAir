// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_ts (MPEG-TS packet header, PSI / SI section parsers). Written by hand; the Rust test rust/onair/tests/headers.rs checks
// it against ffi.rs. Text fields are byte ranges (OnairTsSpan) of the section.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OnairTsSpan { uint32_t off, len; } OnairTsSpan;
typedef struct OnairTsPacketHeader { int32_t pid; uint8_t tei, pusi; int32_t scr, afc, cc, payload_off; } OnairTsPacketHeader;
typedef struct OnairTsPatEntry { int32_t prog, pid; } OnairTsPatEntry;
// kind: 0 data, 1 video, 2 audio, 3 subtitles, 4 teletext.
typedef struct OnairTsPmtStream {
    int32_t stream_type, pid; uint8_t kind, codec_len, has_lang, lang[3], codec[24]; uint32_t sub_first, sub_count;
} OnairTsPmtStream;
typedef struct OnairTsPmtSub { uint8_t teletext, lang[3]; int32_t type, page, anc_page; } OnairTsPmtSub;
typedef struct OnairTsPmtInfo { int32_t prog, pcr_pid; uint8_t ca; uint32_t n_streams, n_subs; } OnairTsPmtInfo;
typedef struct OnairTsSdtService {
    int32_t sid; uint8_t free_ca, has_type, has_provider, has_name; int32_t type; OnairTsSpan provider, name;
} OnairTsSdtService;
typedef struct OnairTsNitLcn { int32_t sid, lcn; } OnairTsNitLcn;
typedef struct OnairTsNitInfo { uint8_t has_name; OnairTsSpan name; uint32_t n_lcn; } OnairTsNitInfo;
typedef struct OnairTsTvct { uint8_t name[8], name_len; int32_t major, minor, prog, stype; } OnairTsTvct;
typedef struct OnairTsTdt { int64_t unix_s; uint8_t text_len, text[32]; } OnairTsTdt;
typedef struct OnairTsEitEvent {
    int32_t event_id; int64_t start; int32_t duration, running, genre; uint8_t lang[3], has_lang, has_title, has_text; uint16_t ext_mask;
    OnairTsSpan title, text, ext[16];
} OnairTsEitEvent;
typedef struct OnairTsEitInfo { int32_t sid, sec_num; uint8_t legacy, has_name; OnairTsSpan now_next_name; uint32_t n_events; } OnairTsEitInfo;

// ABI version of this module (1).
uint32_t onair_ts_version(void);
// sizeof of the structs above in the order PacketHeader, PatEntry, PmtStream, PmtSub, PmtInfo, SdtService, NitLcn, NitInfo, Tvct, Tdt, EitEvent, EitInfo.
void onair_ts_struct_sizes(size_t out[12]);

// MPEG-2 section CRC-32 (polynomial 0x04C11DB7, start 0xFFFFFFFF): 0 over a section that includes its CRC.
uint32_t onair_ts_crc32(const uint8_t* p, size_t n);
// Header of a transport packet (n >= 4 bytes, sync 0x47); false otherwise. payload_off is -1 when there is no payload.
bool onair_ts_packet_header(const uint8_t* p, size_t n, OnairTsPacketHeader* out);
// The 188 byte PAT packet naming one program (sid at pmt_pid) in transport stream tsid.
void onair_ts_pat_packet(int32_t tsid, int32_t sid, int32_t pmt_pid, int32_t cc, uint8_t* out188);

// Section parsers: the whole section (CRC checked by the caller). Record arrays are (pointer, capacity); the return is the number written (SIZE_MAX / false:
// the section is too short). Text fields are spans of the section.
size_t onair_ts_parse_pat(const uint8_t* s, size_t n, int32_t* tsid, OnairTsPatEntry* out, size_t cap);
bool onair_ts_parse_pmt(const uint8_t* s, size_t n, OnairTsPmtInfo* info, OnairTsPmtStream* streams, size_t scap, OnairTsPmtSub* subs, size_t bcap);
size_t onair_ts_parse_sdt(const uint8_t* s, size_t n, int32_t* tsid, int32_t* onid, OnairTsSdtService* out, size_t cap);
bool onair_ts_parse_nit(const uint8_t* s, size_t n, OnairTsNitInfo* info, OnairTsNitLcn* out, size_t cap);
size_t onair_ts_parse_tvct(const uint8_t* s, size_t n, int32_t* tsid, OnairTsTvct* out, size_t cap);
bool onair_ts_parse_tdt(const uint8_t* s, size_t n, OnairTsTdt* out);
bool onair_ts_parse_eit(const uint8_t* s, size_t n, OnairTsEitInfo* info, OnairTsEitEvent* out, size_t cap);

#ifdef __cplusplus
}
#endif
