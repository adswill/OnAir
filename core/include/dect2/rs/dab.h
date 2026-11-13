// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_dab (DAB data layers). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
// Record arrays are (pointer, capacity); parsers return the number of entries found, counted
// past the capacity, SIZE_MAX (or false) when the input is too short. The record layouts are checked at start-up with onair_dab_struct_sizes().
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OnairDabFigRef { int32_t type; uint32_t off, len; } OnairDabFigRef;
typedef struct OnairDabFigHead { int32_t cn, oe, pd, ext; } OnairDabFigHead;
typedef struct OnairDabFig01Sub { int32_t id, start; uint8_t eep, uep_table; int32_t option, level, size, uep_index; } OnairDabFig01Sub;
typedef struct OnairDabFig03Packet { int32_t scid; uint8_t dg; int32_t dscty, sub_id, address; } OnairDabFig03Packet;
typedef struct OnairDabFig02Service { uint32_t sid, first, ncomp; } OnairDabFig02Service;
typedef struct OnairDabFig02Comp { int32_t tmid, ascty, dscty, sub_id, scid; uint8_t primary; } OnairDabFig02Comp;
typedef struct OnairDabFig013App { uint32_t sid; int32_t scids, index, type; uint32_t off, len; } OnairDabFig013App;
typedef struct OnairDabFig017Pty { uint32_t sid; uint8_t sd, primary; int32_t lang, pty; } OnairDabFig017Pty;
typedef struct OnairDabFig018Support { uint32_t sid, asu, first, ncl; } OnairDabFig018Support;
typedef struct OnairDabFig019Switch { int32_t cluster; uint32_t asw; uint8_t new_flag, region; int32_t sub_id; } OnairDabFig019Switch;
typedef struct OnairDabFig010Time { uint32_t mjd; int32_t hour, minute, second; int64_t utc; } OnairDabFig010Time;
typedef struct OnairDabFig1Label { int32_t ext, charset, scids; uint32_t sid, off; } OnairDabFig1Label;
typedef struct OnairDabFig2Seg { int32_t ext, toggle, seg, rfu, scids; uint32_t sid; uint8_t ucs2; int32_t total; uint8_t valid; uint32_t text_off, text_len; } OnairDabFig2Seg;
typedef struct OnairDabMotTime { uint8_t valid, now, long_form; int32_t mjd, hour, minute, second, ms; } OnairDabMotTime;
typedef struct OnairDabRange { uint32_t off, len; } OnairDabRange;
typedef struct OnairDabMotHeader {
    uint64_t body_size, header_size; int32_t content_type, subtype; uint32_t n_params; uint8_t has_name, has_slide, has_trigger, has_expire;
    int32_t category_id, slide_id; OnairDabRange name, category, click_url, alt_url; uint32_t caption_len; OnairDabMotTime trigger, expire;
} OnairDabMotHeader;
typedef struct OnairDabMotParam { int32_t id; OnairDabRange data; } OnairDabMotParam;
// status: 0 ignored, 1 bad CRC or structure, 2 scrambled body, 3 compressed directory, 4 segment (type, seg_no, tid, off / len of the data)
typedef struct OnairDabMotGroup { uint8_t status, last; int32_t type, seg_no, tid; uint32_t off, len; } OnairDabMotGroup;
typedef struct OnairDabDirEntry { int32_t tid; uint32_t off, size; } OnairDabDirEntry;
typedef struct OnairDabPadBytes { uint32_t xlen; uint8_t fpad[2]; } OnairDabPadBytes;
typedef struct OnairDabPadSub { int32_t type; uint32_t off, len; } OnairDabPadSub;
typedef struct OnairDabDlsStep { uint8_t action; uint32_t seg_len; } OnairDabDlsStep;
typedef struct OnairDabDlSegment { int32_t toggle; uint8_t first, last, cmd; int32_t code, seg_no, charset; } OnairDabDlSegment;
typedef struct OnairDabDlpTag { int32_t type, start, len; } OnairDabDlpTag;
typedef struct OnairDabJlGroup { uint8_t status; int32_t type; uint32_t field_len; } OnairDabJlGroup;
typedef struct OnairDabTocEntry { uint32_t id, desc; } OnairDabTocEntry;
typedef struct OnairDabTocInfo { int32_t revision, block_count, block_index, object_count, timeout_min; uint32_t n; } OnairDabTocInfo;
typedef struct OnairDabPacket { uint8_t crc_ok, first, last, cmd; int32_t ci, addr; uint32_t ul; } OnairDabPacket;
typedef struct OnairDabDrmPacket { uint8_t crc_ok, first, last, len_bad; int32_t id, ci; uint32_t n, at; } OnairDabDrmPacket;

// ABI version of this module (1).
uint32_t onair_dab_version(void);
// sizeof of the structs above in the order FigRef, FigHead, Fig01Sub, Fig03Packet, Fig02Service, Fig02Comp, Fig013App, Fig017Pty, Fig018Support, Fig019Switch,
// Fig010Time, Fig1Label, Fig2Seg, MotTime, MotHeader, MotParam, MotGroup, DirEntry, PadBytes, PadSub, DlsStep, DlSegment, DlpTag, JlGroup, TocEntry, TocInfo, Packet, DrmPacket.
void onair_dab_struct_sizes(size_t out[28]);

uint16_t onair_dab_crc16(const uint8_t* p, size_t n);   // CRC-CCITT, start 0xFFFF, inverted

// FIB (30 data bytes) and FIGs.
size_t onair_dab_fib_walk(const uint8_t* fib, size_t n, OnairDabFigRef* out, size_t cap, uint32_t* used);
void onair_dab_fig_head(int32_t type, const uint8_t* d, size_t n, OnairDabFigHead* out);
bool onair_dab_fig0_eid(const uint8_t* d, size_t n, int32_t* eid);
size_t onair_dab_fig0_subchannels(const uint8_t* d, size_t n, OnairDabFig01Sub* out, size_t cap);
size_t onair_dab_fig0_packet(const uint8_t* d, size_t n, OnairDabFig03Packet* out, size_t cap);
size_t onair_dab_fig0_services(const uint8_t* d, size_t n, OnairDabFig02Service* svcs, size_t scap, OnairDabFig02Comp* comps, size_t ccap, size_t* ncomp);
size_t onair_dab_fig0_userapps(const uint8_t* d, size_t n, OnairDabFig013App* out, size_t cap);
size_t onair_dab_fig0_pty(const uint8_t* d, size_t n, OnairDabFig017Pty* out, size_t cap);
size_t onair_dab_fig0_ann_support(const uint8_t* d, size_t n, OnairDabFig018Support* out, size_t cap, uint8_t* clusters, size_t ccap, size_t* ncl);
size_t onair_dab_fig0_ann_switch(const uint8_t* d, size_t n, OnairDabFig019Switch* out, size_t cap);
bool onair_dab_fig0_time(const uint8_t* d, size_t n, OnairDabFig010Time* out);
bool onair_dab_fig1(const uint8_t* d, size_t n, OnairDabFig1Label* out);
bool onair_dab_fig2(const uint8_t* d, size_t n, OnairDabFig2Seg* out);

// MOT.
size_t onair_dab_mot_parse_time(const uint8_t* d, size_t n, OnairDabMotTime* t);
size_t onair_dab_mot_time_text(const OnairDabMotTime* t, uint8_t* out, size_t cap);   // 64 bytes are enough
bool onair_dab_mot_parse_header(const uint8_t* h, size_t n, OnairDabMotHeader* out, OnairDabMotParam* params, size_t pcap, uint8_t* caption, size_t ccap);   // caption: 3 bytes per header byte
size_t onair_dab_mot_content_type_name(int32_t type, int32_t sub, uint8_t* out, size_t cap);   // 24 bytes are enough
void onair_dab_mot_parse_group(const uint8_t* d, size_t n, OnairDabMotGroup* out);
size_t onair_dab_mot_parse_directory(const uint8_t* d, size_t n, OnairDabDirEntry* out, size_t cap);   // SIZE_MAX: shorter than 13 bytes

// PAD.
bool onair_dab_pad_from_au(const uint8_t* au, size_t n, uint8_t* xpad, size_t cap, OnairDabPadBytes* out);   // xpad: 508 bytes
bool onair_dab_pad_from_mp2(const uint8_t* f, size_t n, int32_t* last_cont, uint8_t* xpad, size_t cap, OnairDabPadBytes* out);   // xpad: 196 bytes
size_t onair_dab_pad_split(const uint8_t* xp, size_t n, const uint8_t* fpad2, int32_t* last_type, OnairDabPadSub* out, size_t cap);   // out: 5 entries
uint8_t onair_dab_pad_dg_length(const uint8_t* d, size_t n, int32_t* len);
void onair_dab_pad_dls_step(const uint8_t* d, size_t n, OnairDabDlsStep* out);
void onair_dab_pad_dl_segment(const uint8_t* d, size_t n, OnairDabDlSegment* out);
size_t onair_dab_pad_dlplus_tags(const uint8_t* d, size_t n, OnairDabDlpTag* out4, uint8_t* running);   // SIZE_MAX: too short

// Journaline framing and packet mode.
void onair_dab_jl_data_group(const uint8_t* d, size_t n, OnairDabJlGroup* out);
bool onair_dab_jl_parse_toc(const uint8_t* d, size_t n, OnairDabTocInfo* info, OnairDabTocEntry* out, size_t cap);
bool onair_dab_jl_user_app_data(const uint8_t* d, size_t n, int32_t* version, int32_t* ext_header, int32_t* entry);
void onair_dab_packet(const uint8_t* p, size_t n, OnairDabPacket* out);
void onair_dab_drm_packet(const uint8_t* p, size_t n, OnairDabDrmPacket* out);

#ifdef __cplusplus
}
#endif
