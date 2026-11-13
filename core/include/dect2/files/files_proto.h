// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The pure parsing and writing layers of the files the program reads and writes: CSV (channel lists), SigMF meta files, WAV headers of IQ recordings,
// NORAD two-line elements, the SQLite page / record / cell decoding behind the MBTiles map reader, and the header of the saved results container. Implemented by
// the Rust crate onair_files through core/src/files/files_proto_rs.cpp. The callers
// (iq_file.cpp, iq_record.cpp, channel_db.cpp, passes.cpp, mbtiles.h, results_io.h) keep the file access and the state; tests/files/test_files_proto_rs.cpp is the
// golden-vector test.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::filesp {

// ---------------------------------------------------------------- CSV
// Records of a CSV text (RFC 4180 style: quoted fields may hold , " and line breaks, "" is one quote, CR outside quotes is ignored, blank lines are skipped)
std::vector<std::vector<std::string>> csvParse(const std::string& text);
// The field, quoted when it holds a comma, a quote, CR or LF
std::string csvQuote(const std::string& f);
// "A | B||C" -> A, B, C: items trimmed of space, tab and CR, empty ones dropped
std::vector<std::string> splitList(const std::string& s, char sep);

// ---------------------------------------------------------------- SigMF
struct SigmfMeta {
    int status = 0;                 // 0 ok, 1 not a valid SigMF meta file, 2 no "global" object, 3 no string core:datatype, 4 data type OnAir cannot play (see datatype)
    int format = 0;                 // 0 CS8, 1 CU8, 2 CF32, 3 CI16, 4 CU16 (the order of FileFormat)
    bool bigEndian = false;
    int annotations = 0;
    double sampleRate = 0;          // Hz, 0 = the meta does not say
    double centerHz = 0;            // first capture's frequency, else the middle of the first annotated band, else 0
    uint64_t sampleStart = 0;       // first capture's core:sample_start
    uint64_t headerBytes = 0, trailingBytes = 0;
    std::string description, dataset, datatype;   // core:description, global core:dataset, core:datatype (unescaped; "" when absent)
};
SigmfMeta sigmfParse(const std::string& metaText);
// Meta text of the test tool (writeSigmfMeta): quotes and backslashes of the description escaped, control characters dropped
std::string sigmfToolMeta(const std::string& datatype, double rateHz, double centerHz, uint64_t sampleStart, const std::string& description);
// Meta text of the recorder (sigmfMetaJson): empty dataset / datetime / description, a zero sample count and a centre frequency that is not positive (or 1e18 or more) are left out
std::string sigmfRecorderMeta(const std::string& datatype, double rateHz, double centerHz, const std::string& datetime, const std::string& dataset, uint64_t sampleCount,
                              const std::string& description);

// ---------------------------------------------------------------- WAV
struct WavFmt { uint32_t tag = 0, channels = 0, rate = 0, bits = 0; };
// The body of a `fmt ` chunk of declared length len (n bytes of it, missing bytes read as 0)
WavFmt wavFmt(const uint8_t* body, size_t n, uint32_t len);
// 0 and format (1 CU8, 2 CF32, 3 CI16) for a two channel PCM / float file, 1 when it does not have two channels, 2 for another sample format
int wavClassify(const WavFmt& f, int& format);
// Absolute end of the samples (0 = until the end of the file)
uint64_t wavDataEnd(uint64_t offset, uint32_t len, uint64_t fileSize);
// "..._100.000MHz_..." in a file name -> 100e6, 0 when there is none
double wavCenterFromName(const std::string& name);

// ---------------------------------------------------------------- two-line elements
struct TleData {
    int status = 1;                 // 0 ok, 1 not a TLE (the other fields are empty), 2 the numbers parse but the orbit is unusable (mean motion <= 0 or eccentricity >= 1)
    std::string name;               // trimmed, "0 " of the 3LE form removed, the catalogue number when empty
    int norad = 0;
    double epochJd = 0, bstar = 0, inclDeg = 0, raanDeg = 0, ecc = 0, argpDeg = 0, meanAnomDeg = 0, meanMotion = 0;
};
TleData tleParse(const std::string& line1, const std::string& line2, const std::string& name);
// Every set in a text (name lines optional, blank lines and CR ignored); only those with status 0
std::vector<TleData> tleScan(const std::string& text);

// ---------------------------------------------------------------- SQLite (MBTiles)
struct DbHeader { unsigned status = 0, pageSize = 0, reserved = 0, usable = 0; };   // status 0 ok, 1 not SQLite, 2 bad page size, 3 bad page layout, 4 WAL mode, 5 not UTF-8
DbHeader dbHeader(const uint8_t* h, size_t n);
// SQLite variable-length integer: the bytes used (1..9), 0 when it runs past `end`
int varint(const uint8_t* p, const uint8_t* end, uint64_t& v);
struct Col { bool isNull = true, isInt = false, isBlob = false; int64_t i = 0; std::string s; std::vector<uint8_t> b; };
// A record -> its columns (blobs only copied when wantBlob)
bool record(const std::vector<uint8_t>& r, std::vector<Col>& cols, bool wantBlob = true);
// How much of a payload sits on the b-tree page: false for a payload over 256 MiB; `overflow` when the rest is on overflow pages
bool payloadLocal(uint64_t total, uint64_t usable, bool indexTree, uint64_t& local, bool& overflow);
// Page header at `base` (100 on page 1, else 0)
bool pageHeader(const std::vector<uint8_t>& pg, size_t base, uint8_t& type, unsigned& cells, uint32_t& rightMost);
enum CellKind { kTableLeaf = 13, kTableInterior = 5, kTableInteriorKey = 6, kIndexLeaf = 10, kIndexInterior = 2 };
struct CellInfo { uint32_t child = 0, payloadOff = 0; uint64_t total = 0, key = 0; };
// Cell i of a page (kTableInterior(Key) with i == cells: the right-most child); false for a damaged cell
bool cell(const std::vector<uint8_t>& pg, int kind, size_t base, size_t i, size_t cells, CellInfo& out);
// MBTiles row (TMS) of the XYZ tile; false outside the pyramid
bool tileRow(int z, int x, int y, int64_t& row);
bool isRaster(const std::vector<uint8_t>& d);   // PNG or JPEG
bool isPng(const std::vector<uint8_t>& d);
// The `tiles` CREATE statement lists zoom_level, tile_column, tile_row, tile_data in that order (squashed = the SQL without blanks, quotes and brackets, lower case)
bool tilesSchemaOk(const std::string& sql, std::string& squashed);
// An index (its SQL, empty for the automatic index of a constraint) covers (zoom_level, tile_column, tile_row)
bool indexCovers(const std::string& indexSql, const std::string& tilesSquashed);

// ---------------------------------------------------------------- saved results container (results_io.h)
uint64_t fnv1a(const std::string& s);
uint64_t signatureHash(const std::string& signatureText);       // fnv1a xor the container format
// Offset of the payload of a blob whose header (OARS, format, hash) matches, or SIZE_MAX
size_t containerCheck(const std::string& blob, uint64_t hash);
std::string containerHeader(uint64_t hash);                      // the 16 byte header

} // namespace dect2::filesp
