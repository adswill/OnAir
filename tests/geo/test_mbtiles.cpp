// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MBTiles reader (own SQLite file-format reader) and the coarse world outline. The synthetic file is built here page by page (a table leaf, an
// index leaf, an overflow chain); when the system has the sqlite3 command-line tool, a second, multi-level file made by real SQLite is read too.
#include "dect2/geo/mbtiles.h"
#include "dect2/geo/worldlow.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace dect2;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

typedef std::vector<uint8_t> Bytes;
static void vint(Bytes& b, uint64_t v) {   // SQLite varint (values here are small: 1..3 bytes, 9 not needed)
    uint8_t t[9]; int n = 0;
    t[n++] = v & 0x7F; v >>= 7;
    while (v) { t[n++] = (v & 0x7F) | 0x80; v >>= 7; }
    for (int i = n - 1; i >= 0; i--) b.push_back(t[i]);
}
static void put32(Bytes& b, uint32_t v) { for (int i = 3; i >= 0; i--) b.push_back((v >> (8 * i)) & 0xFF); }
struct Val { int kind; int64_t i; std::string s; Bytes b; };   // 0 int, 1 text, 2 blob
static Val I(int64_t v) { return {0, v, "", {}}; }
static Val T(const std::string& s) { return {1, 0, s, {}}; }
static Val B(const Bytes& b) { return {2, 0, "", b}; }
static Bytes rec(const std::vector<Val>& cols) {
    Bytes hdr, body;
    for (const Val& v : cols) {
        if (v.kind == 0) { vint(hdr, 6); for (int k = 7; k >= 0; k--) body.push_back((uint8_t)((uint64_t)v.i >> (8 * k))); }   // always 8-byte int
        else if (v.kind == 1) { vint(hdr, 13 + 2 * v.s.size()); body.insert(body.end(), v.s.begin(), v.s.end()); }
        else { vint(hdr, 12 + 2 * v.b.size()); body.insert(body.end(), v.b.begin(), v.b.end()); }
    }
    Bytes h; vint(h, hdr.size() + 1);   // header size counts its own byte (all headers here are < 128)
    h.insert(h.end(), hdr.begin(), hdr.end());
    h.insert(h.end(), body.begin(), body.end());
    return h;
}

static const uint32_t PS = 4096;
// A leaf page from ready-made cells (in order). `base` is 100 on page 1.
static Bytes leaf(uint8_t type, const std::vector<Bytes>& cells, size_t base = 0) {
    Bytes pg(PS, 0);
    size_t end = PS;
    std::vector<size_t> off;
    for (const Bytes& c : cells) { end -= c.size(); std::copy(c.begin(), c.end(), pg.begin() + end); off.push_back(end); }
    pg[base] = type;
    pg[base + 3] = (cells.size() >> 8) & 0xFF; pg[base + 4] = cells.size() & 0xFF;
    pg[base + 5] = (end >> 8) & 0xFF; pg[base + 6] = end & 0xFF;
    for (size_t i = 0; i < off.size(); i++) { pg[base + 8 + 2 * i] = (off[i] >> 8) & 0xFF; pg[base + 8 + 2 * i + 1] = off[i] & 0xFF; }
    return pg;
}
static Bytes tableCell(uint64_t rowid, const Bytes& payload, uint32_t overflow = 0, size_t local = 0) {
    Bytes c; vint(c, payload.size()); vint(c, rowid);
    if (!overflow) { c.insert(c.end(), payload.begin(), payload.end()); return c; }
    c.insert(c.end(), payload.begin(), payload.begin() + local); put32(c, overflow); return c;
}
static Bytes indexCell(const Bytes& payload) { Bytes c; vint(c, payload.size()); c.insert(c.end(), payload.begin(), payload.end()); return c; }

static Bytes fakePng(size_t n, uint8_t seed) {
    Bytes b = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (size_t i = 8; i < n; i++) b.push_back((uint8_t)(seed + i * 7));
    return b;
}

static bool writeFile(const std::string& p, const Bytes& d) { std::ofstream f(p, std::ios::binary); f.write((const char*)d.data(), (std::streamsize)d.size()); return (bool)f; }

int main() {
    const fs::path dir = fs::temp_directory_path() / "onair_mbtiles_test";
    fs::create_directories(dir);

    // ---- synthetic file: page 1 master, 2 tiles, 3 index, 4 metadata, 5.. overflow of the big tile
    const Bytes small0 = fakePng(300, 1), small1 = fakePng(500, 2), big = fakePng(9000, 3);
    Bytes bigRec = rec({I(2), I(1), I(1), B(big)});   // z=2 x=1 TMS row 1 -> XYZ y = 2^2-1-1 = 2
    {
        // local part of the big record, as the format defines it for a table leaf
        const uint64_t U = PS, total = bigRec.size(), X = U - 35, M = ((U - 12) * 32 / 255) - 23;
        const uint64_t K = M + ((total - M) % (U - 4));
        const size_t local = (size_t)(K <= X ? K : M);
        std::vector<Bytes> pages;
        // page 1
        std::vector<Bytes> master = {
            tableCell(1, rec({T("table"), T("tiles"), T("tiles"), I(2), T("CREATE TABLE tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob)")})),
            tableCell(2, rec({T("index"), T("tile_index"), T("tiles"), I(3), T("CREATE UNIQUE INDEX tile_index on tiles (zoom_level, tile_column, tile_row)")})),
            tableCell(3, rec({T("table"), T("metadata"), T("metadata"), I(4), T("CREATE TABLE metadata (name text, value text)")})),
        };
        Bytes p1 = leaf(13, master, 100);
        memcpy(p1.data(), "SQLite format 3\0", 16);
        p1[16] = PS >> 8; p1[17] = PS & 0xFF; p1[18] = 1; p1[19] = 1; p1[20] = 0; p1[21] = 64; p1[22] = 32; p1[23] = 32;
        p1[59] = 1;   // text encoding 1 = UTF-8 (offset 56..59)
        // page 2: the tiles
        const Bytes r1 = rec({I(0), I(0), I(0), B(small0)}), r2 = rec({I(1), I(1), I(1), B(small1)});
        Bytes p2 = leaf(13, {tableCell(1, r1), tableCell(2, r2), tableCell(3, bigRec, 5, local)});
        // page 3: the index, sorted by (z, x, row, rowid)
        Bytes p3 = leaf(10, {indexCell(rec({I(0), I(0), I(0), I(1)})), indexCell(rec({I(1), I(1), I(1), I(2)})), indexCell(rec({I(2), I(1), I(1), I(3)}))});
        Bytes p4 = leaf(13, {tableCell(1, rec({T("name"), T("Test map")})), tableCell(2, rec({T("format"), T("png")})), tableCell(3, rec({T("maxzoom"), T("2")}))});
        std::vector<Bytes> ov;   // overflow chain
        size_t pos = local; uint32_t no = 5;
        while (pos < bigRec.size()) {
            const size_t take = std::min<size_t>(bigRec.size() - pos, PS - 4);
            Bytes pg; put32(pg, pos + take < bigRec.size() ? no + 1 : 0);
            pg.insert(pg.end(), bigRec.begin() + pos, bigRec.begin() + pos + take);
            pg.resize(PS, 0);
            ov.push_back(pg); pos += take; no++;
        }
        Bytes file;
        for (const Bytes* p : {&p1, &p2, &p3, &p4}) file.insert(file.end(), p->begin(), p->end());
        for (const Bytes& o : ov) file.insert(file.end(), o.begin(), o.end());
        CHECK(writeFile((dir / "synthetic.mbtiles").string(), file));

        MbTiles mb; std::string err;
        CHECK(mb.open((dir / "synthetic.mbtiles").string(), &err));
        CHECK(mb.hasIndex());
        CHECK(mb.meta("name") == "Test map" && mb.meta("format") == "png" && mb.meta("maxzoom") == "2");
        Bytes t;
        CHECK(mb.tile(0, 0, 0, t) && t == small0);
        CHECK(mb.tile(1, 1, 0, t) && t == small1);   // XYZ y 0 = TMS row 1
        CHECK(mb.tile(2, 1, 2, t) && t == big);      // across overflow pages
        CHECK(MbTiles::isPng(t) && MbTiles::isRaster(t));
        CHECK(!mb.tile(1, 1, 1, t) && t.empty());    // no such row
        CHECK(!mb.tile(5, 0, 0, t));
        CHECK(!mb.tile(-1, 0, 0, t) && !mb.tile(1, 2, 0, t) && !mb.tile(1, 0, 2, t));
        mb.close();
        CHECK(!mb.tile(0, 0, 0, t));
    }

    // ---- damaged and foreign files: false, no crash
    {
        MbTiles mb; std::string err;
        CHECK(!mb.open((dir / "nonexistent.mbtiles").string(), &err));
        CHECK(writeFile((dir / "text.mbtiles").string(), Bytes{'h', 'e', 'l', 'l', 'o'}));
        CHECK(!mb.open((dir / "text.mbtiles").string(), &err) && !err.empty());
        // every truncation and a few byte flips of the good file must be survived
        std::ifstream in((dir / "synthetic.mbtiles").string(), std::ios::binary);
        Bytes good((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (size_t cut : {size_t(100), size_t(4096), size_t(5000), size_t(9000), good.size() - 1}) {
            Bytes d(good.begin(), good.begin() + (long)std::min(cut, good.size()));
            writeFile((dir / "cut.mbtiles").string(), d);
            MbTiles m2; Bytes t;
            if (m2.open((dir / "cut.mbtiles").string())) { m2.tile(0, 0, 0, t); m2.tile(2, 1, 2, t); m2.tile(1, 1, 0, t); }
        }
        unsigned seed = 12345;
        for (int n = 0; n < 300; n++) {
            Bytes d = good;
            for (int k = 0; k < 4; k++) { seed = seed * 1103515245u + 12345u; d[(seed >> 8) % d.size()] = (uint8_t)(seed >> 3); }
            writeFile((dir / "flip.mbtiles").string(), d);
            MbTiles m2; Bytes t;
            if (m2.open((dir / "flip.mbtiles").string())) { m2.tile(0, 0, 0, t); m2.tile(2, 1, 2, t); m2.tile(1, 1, 0, t); m2.tile(1, 0, 1, t); }
        }
    }

    // ---- real SQLite, when its command-line tool is there: thousands of rows, a multi-level index and table, overflow pages
    {
        const std::string db = (dir / "real.mbtiles").string(), sql = (dir / "real.sql").string();
        fs::remove(db);
        {
            std::ofstream s(sql);
            s << "PRAGMA page_size=1024;\n"
                 "CREATE TABLE metadata (name text, value text);\nINSERT INTO metadata VALUES('name','Real'),('maxzoom','7');\n"
                 "CREATE TABLE tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob);\n"
                 "CREATE UNIQUE INDEX tile_index on tiles (zoom_level, tile_column, tile_row);\n"
                 "WITH RECURSIVE x(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM x WHERE n<127) "
                 "INSERT INTO tiles SELECT 7, a.n, b.n, CASE WHEN (a.n*128+b.n)%97=0 THEN zeroblob(5000) ELSE CAST(printf('%08d', a.n*128+b.n) AS BLOB) END FROM x a, x b;\n";
        }
        const std::string cmd = "sqlite3 \"" + db + "\" < \"" + sql + "\" > /dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0 && fs::exists(db)) {
            MbTiles mb; std::string err;
            CHECK(mb.open(db, &err));
            CHECK(mb.hasIndex());
            CHECK(mb.meta("name") == "Real" && mb.meta("maxzoom") == "7");
            int bad = 0, got = 0;
            for (int x = 0; x < 128; x++)
                for (int tmsRow = 0; tmsRow < 128; tmsRow++) {
                    Bytes t;
                    const bool ok = mb.tile(7, x, 127 - tmsRow, t);
                    const int id = x * 128 + tmsRow;
                    if (!ok) { bad++; continue; }
                    got++;
                    if (id % 97 == 0) { if (t.size() != 5000) bad++; }
                    else {
                        char dec[32]; snprintf(dec, sizeof dec, "%08d", id);
                        if (t.size() != 8 || memcmp(t.data(), dec, 8) != 0) bad++;
                    }
                }
            CHECK(got == 128 * 128 && bad == 0);
            Bytes t;
            CHECK(!mb.tile(7, 128, 0, t) && !mb.tile(6, 0, 0, t));
            printf("real sqlite3 file checked: 16384 tiles\n");
        } else printf("sqlite3 tool not available: real-file check skipped\n");
    }

    // ---- world outline
    {
        const auto& w = worldOutline();
        CHECK(w.size() >= 10);
        for (const auto& poly : w) {
            CHECK(poly.size() >= 3);
            for (const LonLat& p : poly) CHECK(p.first >= -180 && p.first <= 180 && p.second >= -90 && p.second <= 90);
        }
        CHECK(worldLandAt(48.9, 2.3));      // Paris
        CHECK(worldLandAt(41.9, -87.6));    // Chicago
        CHECK(worldLandAt(20.0, 10.0));     // Sahara
        CHECK(worldLandAt(-23.5, -46.6));   // Sao Paulo
        CHECK(worldLandAt(-25.0, 134.0));   // central Australia
        CHECK(worldLandAt(55.7, 37.6));     // Moscow
        CHECK(worldLandAt(51.5, -0.1));     // London
        CHECK(!worldLandAt(30.0, -40.0));   // mid-Atlantic
        CHECK(!worldLandAt(0.0, -150.0));   // Pacific
        CHECK(!worldLandAt(-40.0, 80.0));   // Indian Ocean
    }

    fs::remove_all(dir);
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("mbtiles OK\n");
    return 0;
}
