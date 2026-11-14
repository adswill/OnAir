// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_dvbsi (broadcast text character sets, DVB teletext, DVB subtitles). Written by hand; the Rust test rust/onair/tests/headers.rs
// checks it against ffi.rs. Text functions write UTF-8 into a caller buffer and return the
// length, SIZE_MAX when it is too small. Record arrays are (pointer, capacity); parsers return / report the number found, counted past the capacity.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A decoded teletext data unit. kind: 0 ignore (bad framing / address, not counted), 1 header, 2 row 1..24, 3 Fastext (X/27/0), 4 other (counted only).
typedef struct OnairDvbsiTtxUnit {
    uint8_t kind, serial, page_ok, erase, newsflash, subtitle, inhibit, link_act[6];   // link_act: 0 unchanged, 1 cleared, 2 set
    int32_t mag, row, number, subcode, national, link_page[6], link_sub[6];
    uint8_t head[32], row_data[40];
} OnairDvbsiTtxUnit;
typedef struct OnairDvbsiTtxCell { uint32_t ch; uint8_t sextants, fg, bg, dh, mosaic, separated, flash, conceal, boxed; } OnairDvbsiTtxCell;
typedef struct OnairDvbsiSubPesInfo { uint32_t payload_off; uint8_t has_pts; double pts; } OnairDvbsiSubPesInfo;
typedef struct OnairDvbsiSubSegment { int32_t type, page; uint32_t off, len; } OnairDvbsiSubSegment;
typedef struct OnairDvbsiSubPageInfo { int32_t timeout, version, state; uint32_t n_regions; } OnairDvbsiSubPageInfo;
typedef struct OnairDvbsiSubPageRegion { int32_t id, x, y; } OnairDvbsiSubPageRegion;
typedef struct OnairDvbsiSubRegionInfo { int32_t id, version; uint8_t fill; int32_t w, h, depth, clut, pix8, pix4, pix2; uint32_t n_objs; } OnairDvbsiSubRegionInfo;
typedef struct OnairDvbsiSubRegionObject { int32_t id, type, x, y, fg, bg; } OnairDvbsiSubRegionObject;
typedef struct OnairDvbsiSubClutEntry { int32_t id, flags; uint32_t rgba; } OnairDvbsiSubClutEntry;
typedef struct OnairDvbsiSubObjectInfo { int32_t id; uint32_t top_off, top_len, bot_off, bot_len; } OnairDvbsiSubObjectInfo;

// ABI version of this module (1).
uint32_t onair_dvbsi_version(void);
// sizeof of the structs above in the order TtxUnit, TtxCell, SubPesInfo, SubSegment, SubPageInfo, SubPageRegion, SubRegionInfo, SubRegionObject, SubClutEntry, SubObjectInfo.
void onair_dvbsi_struct_sizes(size_t out[10]);

// Character set conversion routine for the tables that need iconv: (charset name, NUL terminated; source; length; output; capacity) -> bytes written, SIZE_MAX
// = failed (the raw bytes are kept). Without one registered the raw bytes are kept.
typedef size_t (*OnairDvbsiConvert)(const uint8_t* charset, const uint8_t* src, size_t n, uint8_t* out, size_t cap);
void onair_dvbsi_set_convert(OnairDvbsiConvert f);

// Text to UTF-8. ebu_latin, dab_charset, iso6937 need 3 * n bytes; ucs2be 3 * n; clean n; dvb_text 4 * n + 16.
size_t onair_dvbsi_ebu_latin(const uint8_t* p, size_t n, uint8_t* out, size_t cap);
size_t onair_dvbsi_ucs2be(const uint8_t* p, size_t n, uint8_t* out, size_t cap);
size_t onair_dvbsi_dab_charset(int32_t cs, const uint8_t* p, size_t n, uint8_t* out, size_t cap);
size_t onair_dvbsi_iso6937(const uint8_t* p, size_t n, uint8_t* out, size_t cap);
size_t onair_dvbsi_clean(const uint8_t* p, size_t n, uint8_t* out, size_t cap);
size_t onair_dvbsi_dvb_text(const uint8_t* p, size_t n, uint32_t gb_default, uint8_t* out, size_t cap);
// Generic fallback: 0 DVB/ISO 6937, 1 GBK, 2 EUC-KR, 3 Big5, 4 UTF-8, 5 ISO 8859-15. The older bool-like entry point remains ABI compatible.
size_t onair_dvbsi_dvb_text_with_encoding(const uint8_t* p, size_t n, uint32_t encoding, uint8_t* out, size_t cap);

// Teletext (EN 300 706): Hamming 8/4 (nibble or -1), Hamming 24/18 of 3 bytes (-1 for two errors), bit reversal, G0 code to Unicode in a national subset.
int32_t onair_dvbsi_ttx_hamming84(uint32_t b);
int32_t onair_dvbsi_ttx_hamming2418(const uint8_t* b3);
uint8_t onair_dvbsi_ttx_reverse8(uint32_t b);
uint32_t onair_dvbsi_ttx_g0(uint32_t code, int32_t national);
// Offsets of the 44 byte data units of a teletext PES packet (the count found, also past the capacity).
size_t onair_dvbsi_ttx_pes_units(const uint8_t* pes, size_t n, uint32_t* out, size_t cap);
void onair_dvbsi_ttx_decode_unit(const uint8_t* u44, OnairDvbsiTtxUnit* out);
// 25 x 40 characters (7 bit) and the mask of received rows to 25 x 40 cells; one row of cells as UTF-8 (120 bytes).
void onair_dvbsi_ttx_render(const uint8_t* rows, uint32_t row_mask, int32_t national, OnairDvbsiTtxCell* out);
size_t onair_dvbsi_ttx_row_text(const OnairDvbsiTtxCell* cells, int32_t row, uint8_t* out, size_t cap);

// DVB subtitles (EN 300 743).
bool onair_dvbsi_sub_pes_info(const uint8_t* pes, size_t n, OnairDvbsiSubPesInfo* out);
size_t onair_dvbsi_sub_segments(const uint8_t* d, size_t n, OnairDvbsiSubSegment* out, size_t cap);
bool onair_dvbsi_sub_page_composition(const uint8_t* d, size_t n, OnairDvbsiSubPageInfo* info, OnairDvbsiSubPageRegion* out, size_t cap);
bool onair_dvbsi_sub_region_composition(const uint8_t* d, size_t n, OnairDvbsiSubRegionInfo* info, OnairDvbsiSubRegionObject* out, size_t cap);
size_t onair_dvbsi_sub_clut_definition(const uint8_t* d, size_t n, int32_t* id, OnairDvbsiSubClutEntry* out, size_t cap);   // SIZE_MAX: too short
bool onair_dvbsi_sub_display_definition(const uint8_t* d, size_t n, int32_t* w, int32_t* h);
bool onair_dvbsi_sub_object_data(const uint8_t* d, size_t n, OnairDvbsiSubObjectInfo* out);
void onair_dvbsi_sub_default_clut(uint32_t* e2, uint32_t* e4, uint32_t* e8);   // 4, 16, 256 entries
uint32_t onair_dvbsi_sub_ycrcb(int32_t y, int32_t cr, int32_t cb, int32_t t);
// Pixel data of one field into a region of reg_w * reg_h palette indices.
void onair_dvbsi_sub_decode_field(const uint8_t* f, size_t n, int32_t x0, int32_t y0, int32_t line_step, uint32_t double_lines, int32_t reg_w, int32_t reg_h,
                                  int32_t reg_depth, uint8_t* pix);
void onair_dvbsi_sub_region_rgba(const uint8_t* pix, size_t n, int32_t depth, const uint32_t* e2, const uint32_t* e4, const uint32_t* e8, uint8_t* out);   // 4 * n bytes
void onair_dvbsi_sub_blend_region(uint8_t* out, int32_t dw, int32_t dh, int32_t rx, int32_t ry, int32_t rw, int32_t rh, const uint8_t* rgba);
double onair_dvbsi_sub_pts_delta(double a, double b);

#ifdef __cplusplus
}
#endif
