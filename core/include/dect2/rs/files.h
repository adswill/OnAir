// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_files (file formats). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ABI version of this module (1).
uint32_t onair_files_version(void);

// A byte range of a text the caller gave.
typedef struct OnairFilesSpan { uint32_t off, len; } OnairFilesSpan;
typedef struct OnairFilesSigmf {
    int32_t status;        // 0 ok, 1 not valid, 2 no global object, 3 no string core:datatype, 4 unsupported data type
    int32_t format;        // 0 CS8, 1 CU8, 2 CF32, 3 CI16, 4 CU16
    int32_t big_endian;
    int32_t annotations;
    double sample_rate, center_hz;
    uint64_t sample_start, header_bytes, trailing_bytes;
    OnairFilesSpan description, dataset, datatype;   // raw (escaped) ranges of the text; len 0 when absent
} OnairFilesSigmf;
typedef struct OnairFilesWavFmt { uint32_t tag, channels, rate, bits; } OnairFilesWavFmt;
typedef struct OnairFilesTle {
    int32_t status;        // 0 ok, 1 not a TLE, 2 unusable orbit
    int32_t norad;
    int32_t name_is_norad; // 1: the name is the catalogue number
    OnairFilesSpan name;
    double epoch_jd, bstar, incl_deg, raan_deg, ecc, argp_deg, mean_anom_deg, mean_motion;
} OnairFilesTle;
typedef struct OnairFilesDbHeader { uint32_t status, page_size, reserved, usable; } OnairFilesDbHeader;
typedef struct OnairFilesCol { uint32_t kind; int64_t value; OnairFilesSpan data; } OnairFilesCol;   // kind: 0 NULL, 1 integer, 2 float, 3 text, 4 blob
typedef struct OnairFilesCell { uint32_t child, payload_off; uint64_t total, key; } OnairFilesCell;

// sizeof of the structs above in the order Sigmf, WavFmt, Tle, DbHeader, Col, Cell, written to out[0..6]; the C++ side compares them at start-up.
void onair_files_struct_sizes(size_t* out);

// ---- CSV
// out[0] = records, out[1] = fields, out[2] = bytes of field text (quotes removed). False for a null out.
bool onair_files_csv_measure(const uint8_t* text, size_t n, size_t* out);
// data = the field bytes back to back, ends[k] = end offset (in data) of field k, counts[r] = fields of record r; false when an array is shorter than measured.
bool onair_files_csv_parse(const uint8_t* text, size_t n, uint8_t* data, size_t n_data, uint32_t* ends, size_t n_ends, uint32_t* counts, size_t n_counts);
size_t onair_files_csv_quote_len(const uint8_t* f, size_t n);
// Returns the length or SIZE_MAX when out is too short.
size_t onair_files_csv_quote(const uint8_t* f, size_t n, uint8_t* out, size_t n_out);
// Items of a sep separated list, trimmed (space, tab, CR), empty ones dropped: ends[2k], ends[2k+1] = start, end of item k. Returns the item count (SIZE_MAX if ends is
// too short; an empty ends only counts).
size_t onair_files_split_list(const uint8_t* s, size_t n, uint32_t sep, uint32_t* ends, size_t n_ends);

// ---- JSON / SigMF
// Resolves the escapes of a JSON string's raw bytes; returns the full length of the result (never more than n).
size_t onair_files_json_unescape(const uint8_t* raw, size_t n, uint8_t* out, size_t n_out);
void onair_files_sigmf_parse(const uint8_t* text, size_t n, OnairFilesSigmf* out);
// SigMF data type name -> format (0 CS8, 1 CU8, 2 CF32, 3 CI16, 4 CU16) and endianness; false for an unsupported one.
bool onair_files_sigmf_datatype(const uint8_t* name, size_t n, int32_t* fmt, int32_t* big_endian);
// The meta texts; each returns the text length (the part that fits in out is written, the rest only counted).
size_t onair_files_sigmf_write_tool(const uint8_t* datatype, size_t n_dt, double rate, double center, uint64_t sample_start, const uint8_t* desc, size_t n_desc, uint8_t* out, size_t n_out);
size_t onair_files_sigmf_write_recorder(const uint8_t* datatype, size_t n_dt, double rate, double center, const uint8_t* datetime, size_t n_datetime, const uint8_t* dataset,
                                        size_t n_dataset, uint64_t sample_count, const uint8_t* desc, size_t n_desc, uint8_t* out, size_t n_out);

// ---- WAV
void onair_files_wav_fmt(const uint8_t* body, size_t n, uint32_t len, OnairFilesWavFmt* out);
// 0 (and *format: 1 CU8, 2 CF32, 3 CI16), 1 = not two channels, 2 = unsupported sample format.
uint32_t onair_files_wav_classify(const OnairFilesWavFmt* f, int32_t* format);
uint64_t onair_files_wav_data_end(uint64_t offset, uint32_t len, uint64_t file_size);
double onair_files_wav_center_from_name(const uint8_t* name, size_t n);

// ---- two-line elements
// name range is relative to the name text passed in
void onair_files_tle_parse(const uint8_t* l1, size_t n1, const uint8_t* l2, size_t n2, const uint8_t* name, size_t n_name, OnairFilesTle* out);
// All sets of a text (status 0 only), name ranges relative to text. Returns the count (more than cap = out was too short).
size_t onair_files_tle_scan(const uint8_t* text, size_t n, OnairFilesTle* out, size_t cap);

// ---- SQLite (MBTiles)
void onair_files_sqlite_header(const uint8_t* h, size_t n, OnairFilesDbHeader* out);
// Bytes used (1..9) and *v, 0 when p ends first.
int32_t onair_files_sqlite_varint(const uint8_t* p, size_t n, uint64_t* v);
// Column count (more than cap = cols too short), SIZE_MAX for a damaged record.
size_t onair_files_sqlite_record(const uint8_t* r, size_t n, OnairFilesCol* cols, size_t cap);
// 0 rejected, 1 all of the payload is on the page (*local = total), 2 *local bytes on the page, the rest on overflow pages.
int32_t onair_files_sqlite_payload_local(uint64_t total, uint64_t usable, uint32_t index_tree, uint64_t* local);
// out[0] = page type, out[1] = cell count, out[2] = right-most child.
bool onair_files_sqlite_page_header(const uint8_t* pg, size_t n, size_t base, uint32_t* out);
// kind: 13 table leaf, 5 table interior, 6 table interior with key, 10 index leaf, 2 index interior.
bool onair_files_sqlite_cell(const uint8_t* pg, size_t n, uint32_t kind, size_t base, size_t i, size_t cells, OnairFilesCell* out);
bool onair_files_sqlite_tile_row(int32_t z, int32_t x, int32_t y, int64_t* row);
bool onair_files_sqlite_is_raster(const uint8_t* d, size_t n);
bool onair_files_sqlite_is_png(const uint8_t* d, size_t n);
// scratch (>= n bytes) receives the squashed SQL; returns its length or SIZE_MAX when the column order is not the standard one.
size_t onair_files_sqlite_tiles_schema(const uint8_t* sql, size_t n, uint8_t* scratch, size_t n_scratch);
bool onair_files_sqlite_index_covers(const uint8_t* sql, size_t n, uint8_t* scratch, size_t n_scratch, const uint8_t* tiles_squashed, size_t n_tiles);

// ---- results container
uint64_t onair_files_fnv1a(const uint8_t* p, size_t n);
uint64_t onair_files_signature_hash(const uint8_t* sig, size_t n);
// Payload offset (16) of a blob whose header matches the hash, SIZE_MAX otherwise.
size_t onair_files_container_check(const uint8_t* blob, size_t n, uint64_t hash);
bool onair_files_container_header(uint64_t hash, uint8_t* out, size_t n_out);

#ifdef __cplusplus
}
#endif
