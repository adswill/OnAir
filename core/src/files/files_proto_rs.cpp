// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// files_proto.h on top of the Rust crate rust/onair_files: thin wrappers over the C ABI of core/include/dect2/rs/files.h. The code is what
// used to sit in iq_file.cpp, iq_record.cpp, channel_db.cpp, passes.cpp, mbtiles.h and results_io.h, moved to Rust; the changes made on the way:
//  * numbers read from text (SigMF meta, TLE fields) are decimal tokens only, [+-]? (digits [. digits*] | . digits) ([eE] [+-]? digits)?, converted by strtod: hex floats,
//    "inf" and "nan" are no numbers (strtod used to accept them);
//  * the SigMF meta writers do not cut long descriptions at 1023 bytes, a rate or frequency that is not finite is written as 0 (it was "nan" / "inf", not JSON) and
//    the recorder leaves out a centre frequency of 1e18 or more (the %.1f text of 1e300 was cut at 95 digits);
//  * a double that does not fit a 64 bit unsigned (SigMF sample_start, header_bytes) saturates, it was undefined;
//  * the b-tree code refuses a usable page size under 36 bytes and checks the page is long enough for its header.
#include "dect2/files/files_proto.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/files.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2::filesp {

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[6];
        onair_files_struct_sizes(s);
        dect2::rsAbiCheck("onair_files", s, {sizeof(OnairFilesSigmf), sizeof(OnairFilesWavFmt), sizeof(OnairFilesTle), sizeof(OnairFilesDbHeader), sizeof(OnairFilesCol), sizeof(OnairFilesCell)});
    }
} abiCheck;
inline const uint8_t* U(const std::string& s) { return (const uint8_t*)s.data(); }
inline const uint8_t* U(const std::vector<uint8_t>& v) { return v.data(); }
// the text of a raw JSON string range with its escapes resolved
std::string unescaped(const std::string& text, OnairFilesSpan sp) {
    if (!sp.len) return std::string();
    const uint8_t* raw = U(text) + sp.off;
    std::string out(sp.len, '\0');
    out.resize(onair_files_json_unescape(raw, sp.len, (uint8_t*)out.data(), out.size()));
    return out;
}
} // namespace

std::vector<std::vector<std::string>> csvParse(const std::string& t) {
    std::vector<std::vector<std::string>> out;
    size_t sz[3];
    if (!onair_files_csv_measure(U(t), t.size(), sz)) return out;
    std::vector<uint8_t> data(sz[2]);
    std::vector<uint32_t> ends(sz[1]), counts(sz[0]);
    if (!onair_files_csv_parse(U(t), t.size(), data.data(), data.size(), ends.data(), ends.size(), counts.data(), counts.size())) return out;
    size_t f = 0;
    uint32_t prev = 0;
    for (size_t r = 0; r < counts.size(); r++) {
        std::vector<std::string> rec;
        for (uint32_t k = 0; k < counts[r]; k++, f++) { rec.emplace_back((const char*)data.data() + prev, ends[f] - prev); prev = ends[f]; }
        out.push_back(std::move(rec));
    }
    return out;
}

std::string csvQuote(const std::string& f) {
    std::string out(onair_files_csv_quote_len(U(f), f.size()), '\0');
    onair_files_csv_quote(U(f), f.size(), (uint8_t*)out.data(), out.size());
    return out;
}

std::vector<std::string> splitList(const std::string& s, char sep) {
    std::vector<std::string> v;
    const size_t n = onair_files_split_list(U(s), s.size(), (uint8_t)sep, nullptr, 0);
    std::vector<uint32_t> ends(2 * n);
    if (n == SIZE_MAX || onair_files_split_list(U(s), s.size(), (uint8_t)sep, ends.data(), ends.size()) != n) return v;
    for (size_t k = 0; k < n; k++) v.emplace_back(s.substr(ends[2 * k], ends[2 * k + 1] - ends[2 * k]));
    return v;
}

SigmfMeta sigmfParse(const std::string& t) {
    OnairFilesSigmf c;
    onair_files_sigmf_parse(U(t), t.size(), &c);
    SigmfMeta m;
    m.status = c.status; m.format = c.format; m.bigEndian = c.big_endian != 0; m.annotations = c.annotations;
    m.sampleRate = c.sample_rate; m.centerHz = c.center_hz; m.sampleStart = c.sample_start; m.headerBytes = c.header_bytes; m.trailingBytes = c.trailing_bytes;
    m.description = unescaped(t, c.description); m.dataset = unescaped(t, c.dataset); m.datatype = unescaped(t, c.datatype);
    return m;
}

std::string sigmfToolMeta(const std::string& dt, double rate, double center, uint64_t sampleStart, const std::string& desc) {
    const size_t n = onair_files_sigmf_write_tool(U(dt), dt.size(), rate, center, sampleStart, U(desc), desc.size(), nullptr, 0);
    std::string out(n, '\0');
    onair_files_sigmf_write_tool(U(dt), dt.size(), rate, center, sampleStart, U(desc), desc.size(), (uint8_t*)out.data(), out.size());
    return out;
}

std::string sigmfRecorderMeta(const std::string& dt, double rate, double center, const std::string& datetime, const std::string& dataset, uint64_t sampleCount,
                              const std::string& desc) {
    const size_t n = onair_files_sigmf_write_recorder(U(dt), dt.size(), rate, center, U(datetime), datetime.size(), U(dataset), dataset.size(), sampleCount, U(desc), desc.size(), nullptr, 0);
    std::string out(n, '\0');
    onair_files_sigmf_write_recorder(U(dt), dt.size(), rate, center, U(datetime), datetime.size(), U(dataset), dataset.size(), sampleCount, U(desc), desc.size(), (uint8_t*)out.data(), out.size());
    return out;
}

WavFmt wavFmt(const uint8_t* body, size_t n, uint32_t len) {
    OnairFilesWavFmt c;
    onair_files_wav_fmt(body, n, len, &c);
    return WavFmt{c.tag, c.channels, c.rate, c.bits};
}
int wavClassify(const WavFmt& f, int& format) {
    OnairFilesWavFmt c = {f.tag, f.channels, f.rate, f.bits};
    int32_t fm = 0;
    const uint32_t code = onair_files_wav_classify(&c, &fm);
    if (code == 0) format = fm;
    return (int)code;
}
uint64_t wavDataEnd(uint64_t offset, uint32_t len, uint64_t size) { return onair_files_wav_data_end(offset, len, size); }
double wavCenterFromName(const std::string& name) { return onair_files_wav_center_from_name(U(name), name.size()); }

namespace {
TleData fromC(const OnairFilesTle& c, const std::string& nameText) {
    TleData t;
    t.status = c.status;
    if (c.status == 1) return t;
    t.norad = c.norad;
    t.name = c.name_is_norad ? std::to_string(c.norad) : nameText.substr(c.name.off, c.name.len);
    t.epochJd = c.epoch_jd; t.bstar = c.bstar; t.inclDeg = c.incl_deg; t.raanDeg = c.raan_deg; t.ecc = c.ecc; t.argpDeg = c.argp_deg; t.meanAnomDeg = c.mean_anom_deg; t.meanMotion = c.mean_motion;
    return t;
}
} // namespace

TleData tleParse(const std::string& l1, const std::string& l2, const std::string& name) {
    OnairFilesTle c;
    onair_files_tle_parse(U(l1), l1.size(), U(l2), l2.size(), U(name), name.size(), &c);
    return fromC(c, name);
}

std::vector<TleData> tleScan(const std::string& text) {
    std::vector<OnairFilesTle> v(text.size() / 100 + 1);
    size_t n = onair_files_tle_scan(U(text), text.size(), v.data(), v.size());
    if (n > v.size()) { v.resize(n); n = onair_files_tle_scan(U(text), text.size(), v.data(), v.size()); }
    std::vector<TleData> out;
    for (size_t i = 0; i < n && i < v.size(); i++) out.push_back(fromC(v[i], text));
    return out;
}

DbHeader dbHeader(const uint8_t* h, size_t n) {
    OnairFilesDbHeader c;
    onair_files_sqlite_header(h, n, &c);
    return DbHeader{c.status, c.page_size, c.reserved, c.usable};
}

int varint(const uint8_t* p, const uint8_t* end, uint64_t& v) { return onair_files_sqlite_varint(p, end > p ? (size_t)(end - p) : 0, &v); }

bool record(const std::vector<uint8_t>& r, std::vector<Col>& cols, bool wantBlob) {
    cols.clear();
    size_t n = onair_files_sqlite_record(r.data(), r.size(), nullptr, 0);
    if (n == SIZE_MAX) return false;
    std::vector<OnairFilesCol> c(n);
    if (onair_files_sqlite_record(r.data(), r.size(), c.data(), c.size()) != n) return false;
    for (const auto& x : c) {
        Col k;
        k.isNull = x.kind == 0;
        k.isInt = x.kind == 1;
        k.i = x.kind == 1 ? x.value : 0;
        if (x.kind == 3) k.s.assign((const char*)r.data() + x.data.off, x.data.len);
        else if (x.kind == 4) { k.isBlob = true; if (wantBlob) k.b.assign(r.begin() + x.data.off, r.begin() + x.data.off + x.data.len); }
        cols.push_back(std::move(k));
    }
    return true;
}

bool payloadLocal(uint64_t total, uint64_t usable, bool indexTree, uint64_t& local, bool& overflow) {
    uint64_t l = 0;
    const int r = onair_files_sqlite_payload_local(total, usable, indexTree, &l);
    if (!r) return false;
    local = l; overflow = r == 2;
    return true;
}

bool pageHeader(const std::vector<uint8_t>& pg, size_t base, uint8_t& type, unsigned& cells, uint32_t& rightMost) {
    uint32_t o[3];
    if (!onair_files_sqlite_page_header(pg.data(), pg.size(), base, o)) return false;
    type = (uint8_t)o[0]; cells = o[1]; rightMost = o[2];
    return true;
}

bool cell(const std::vector<uint8_t>& pg, int kind, size_t base, size_t i, size_t cells, CellInfo& out) {
    OnairFilesCell c;
    out = CellInfo();
    if (kind < 0 || kind > 255 || !onair_files_sqlite_cell(pg.data(), pg.size(), (uint8_t)kind, base, i, cells, &c)) return false;
    out.child = c.child; out.payloadOff = c.payload_off; out.total = c.total; out.key = c.key;
    return true;
}

bool tileRow(int z, int x, int y, int64_t& row) { int64_t r = 0; if (!onair_files_sqlite_tile_row(z, x, y, &r)) return false; row = r; return true; }
bool isRaster(const std::vector<uint8_t>& d) { return onair_files_sqlite_is_raster(d.data(), d.size()); }
bool isPng(const std::vector<uint8_t>& d) { return onair_files_sqlite_is_png(d.data(), d.size()); }

bool tilesSchemaOk(const std::string& sql, std::string& squashed) {
    std::string s(sql.size(), '\0');
    const size_t n = onair_files_sqlite_tiles_schema(U(sql), sql.size(), (uint8_t*)s.data(), s.size());
    if (n == SIZE_MAX) { squashed.clear(); return false; }
    s.resize(n);
    squashed = std::move(s);
    return true;
}

bool indexCovers(const std::string& sql, const std::string& tilesSquashed) {
    std::string scratch(sql.size(), '\0');
    return onair_files_sqlite_index_covers(U(sql), sql.size(), (uint8_t*)scratch.data(), scratch.size(), U(tilesSquashed), tilesSquashed.size());
}

uint64_t fnv1a(const std::string& s) { return onair_files_fnv1a(U(s), s.size()); }
uint64_t signatureHash(const std::string& s) { return onair_files_signature_hash(U(s), s.size()); }
size_t containerCheck(const std::string& blob, uint64_t hash) { return onair_files_container_check(U(blob), blob.size(), hash); }
std::string containerHeader(uint64_t hash) {
    std::string s(16, '\0');
    onair_files_container_header(hash, (uint8_t*)s.data(), s.size());
    return s;
}

} // namespace dect2::filesp
