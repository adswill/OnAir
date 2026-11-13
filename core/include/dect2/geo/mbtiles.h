// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Read-only MBTiles reader (a SQLite file with a `tiles` table of zoom_level, tile_column, tile_row, tile_data), written from the public
// SQLite file-format description so no SQLite library is needed (it works the same on every platform).
//
// What it reads: the b-trees of the `tiles` table and, when there is one on (zoom_level, tile_column, tile_row), its index, so a lookup
// touches a handful of pages. Without such an index a lookup scans the table (slow for big files, still correct). Overflow pages are followed.
// What it does not read: the "deduplicated" layout where `tiles` is a view over `map` and `images`; WAL-mode files with a live -wal; databases
// that are not UTF-8/page-size sane. Tile bytes are returned as stored (PNG or JPEG for raster maps; a vector .pbf is gzip data and is of no
// use to the raster map, see isRaster()). Every read is bounds checked: a damaged file gives false, never a crash.
// MBTiles stores rows in TMS order: tile_row = 2^z - 1 - y for the usual XYZ y of the map; tile() takes the XYZ y.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "dect2/common/endian.h"
#include "dect2/files/files_proto.h"

namespace dect2 {

class MbTiles {
public:
    MbTiles() = default;
    ~MbTiles() { close(); }
    MbTiles(const MbTiles&) = delete;
    MbTiles& operator=(const MbTiles&) = delete;

    bool open(const std::string& path, std::string* err = nullptr) {
        close();
        std::lock_guard<std::mutex> lk(mu_);
        f_ = fopen(path.c_str(), "rb");
        if (!f_) return fail(err, "cannot open the file");
        uint8_t h[100];
        // the 100 byte header: core/src/files/files_proto.cpp (C++) or the Rust crate onair_files
        if (fread(h, 1, 100, f_) != 100) return closeFail(err, "not an SQLite file");
        const filesp::DbHeader dh = filesp::dbHeader(h, 100);
        if (dh.status == 1) return closeFail(err, "not an SQLite file");
        if (dh.status == 2) return closeFail(err, "bad SQLite page size");
        if (dh.status == 3) return closeFail(err, "bad SQLite page layout");
        if (dh.status == 4) return closeFail(err, "WAL-mode file: open it once in a tool that checkpoints it");
        if (dh.status == 5) return closeFail(err, "text is not UTF-8");
        pageSize_ = dh.pageSize; reserved_ = dh.reserved; usable_ = dh.usable;
        fseek(f_, 0, SEEK_END);
        const long sz = ftell(f_);
        pages_ = sz > 0 ? (uint32_t)(sz / pageSize_) : 0;
        if (pages_ == 0) return closeFail(err, "empty file");
        if (!readSchema()) return closeFail(err, "no usable `tiles` table (a deduplicated map/images layout is not supported)");
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lk(mu_);
        if (f_) fclose(f_);
        f_ = nullptr; tilesRoot_ = indexRoot_ = 0; meta_.clear();
    }
    bool isOpen() const { return f_ != nullptr; }
    bool hasIndex() const { return indexRoot_ != 0; }

    // The tile (XYZ y) as stored; false when there is none
    bool tile(int z, int x, int y, std::vector<uint8_t>& out) {
        std::lock_guard<std::mutex> lk(mu_);
        out.clear();
        int64_t row = 0;
        if (!f_ || !filesp::tileRow(z, x, y, row)) return false;
        if (indexRoot_) {
            int64_t rowid = 0;
            if (!indexFind(z, x, row, rowid)) return false;
            return tableRowTile(rowid, out);
        }
        return scanTile(z, x, row, out);
    }

    // The rows of the `metadata` table (name -> value), e.g. "name", "format", "minzoom", "maxzoom", "attribution"
    std::string meta(const std::string& key) const {
        for (const auto& kv : meta_) if (kv.first == key) return kv.second;
        return std::string();
    }
    // True when the bytes are a picture the raster map can draw (PNG or JPEG), not a vector tile
    static bool isRaster(const std::vector<uint8_t>& d) { return filesp::isRaster(d); }
    static bool isPng(const std::vector<uint8_t>& d) { return filesp::isPng(d); }

    // varint of the SQLite record format: up to 9 bytes. Returns the bytes used (0 when it runs past `end`).
    static int varint(const uint8_t* p, const uint8_t* end, uint64_t& v) { return filesp::varint(p, end, v); }

private:
    using Col = filesp::Col;

    FILE* f_ = nullptr;
    std::mutex mu_;
    uint32_t pageSize_ = 0, usable_ = 0, pages_ = 0, reserved_ = 0;
    uint32_t tilesRoot_ = 0, indexRoot_ = 0;
    std::vector<std::pair<std::string, std::string>> meta_;

    static bool fail(std::string* e, const char* m) { if (e) *e = m; return false; }
    bool closeFail(std::string* e, const char* m) { if (f_) fclose(f_); f_ = nullptr; return fail(e, m); }
    static uint32_t be32(const uint8_t* p) { return (uint32_t)dect2::endian::loadBe32(p); }
    static uint32_t be16(const uint8_t* p) { return (uint32_t)dect2::endian::loadBe16(p); }

    bool page(uint32_t no, std::vector<uint8_t>& buf) {
        if (no < 1 || no > pages_) return false;
        buf.resize(pageSize_);
        if (fseek(f_, (long)((int64_t)(no - 1) * pageSize_), SEEK_SET) != 0) return false;
        return fread(buf.data(), 1, pageSize_, f_) == pageSize_;
    }

    // One b-tree cell's payload, overflow pages followed. `p` points into `pg`; payload starts at `at`.
    bool payload(const std::vector<uint8_t>& pg, size_t at, uint64_t total, bool indexTree, std::vector<uint8_t>& out) {
        out.clear();
        const uint64_t U = usable_;
        uint64_t local = 0;
        bool overflow = false;
        if (!filesp::payloadLocal(total, U, indexTree, local, overflow)) return false;   // how much is on this page
        if (at + local + (overflow ? 4 : 0) > pg.size()) return false;
        out.assign(pg.begin() + at, pg.begin() + at + local);
        if (overflow) {
            uint32_t next = be32(&pg[at + local]);
            std::vector<uint8_t> ov;
            int hops = 0;
            while (out.size() < total) {
                if (next == 0 || ++hops > 1000000 || !page(next, ov)) return false;
                const size_t take = (size_t)std::min<uint64_t>(total - out.size(), U - 4);
                out.insert(out.end(), ov.begin() + 4, ov.begin() + 4 + take);
                next = be32(ov.data());
            }
        }
        return true;
    }

    // A record -> its columns (blobs only copied when wantBlob)
    static bool record(const std::vector<uint8_t>& r, std::vector<Col>& cols, bool wantBlob = true) { return filesp::record(r, cols, wantBlob); }

    // Visit every row of a table b-tree (rowid, record); stops when fn returns false. Depth limited against loops.
    using RowFn = std::function<bool(int64_t, const std::vector<uint8_t>&)>;
    bool walkTable(uint32_t root, const RowFn& fn, int depth = 0) {
        if (depth > 20) return false;
        std::vector<uint8_t> pg;
        if (!page(root, pg)) return false;
        const size_t base = root == 1 ? 100 : 0;
        uint8_t type; unsigned cells; uint32_t rightMost;
        if (!filesp::pageHeader(pg, base, type, cells, rightMost)) return false;
        if (type == 13) {
            for (unsigned i = 0; i < cells; i++) {
                filesp::CellInfo c;
                if (!filesp::cell(pg, filesp::kTableLeaf, base, i, cells, c)) return false;
                std::vector<uint8_t> rec;
                if (!payload(pg, c.payloadOff, c.total, false, rec)) return false;
                if (!fn((int64_t)c.key, rec)) return true;
            }
            return true;
        }
        if (type == 5) {
            for (unsigned i = 0; i <= cells; i++) {
                filesp::CellInfo c;
                if (!filesp::cell(pg, filesp::kTableInterior, base, i, cells, c)) return false;
                bool stop = false;
                if (!walkTable(c.child, [&](int64_t id, const std::vector<uint8_t>& r) { if (!fn(id, r)) { stop = true; return false; } return true; }, depth + 1)) return false;
                if (stop) return true;
            }
            return true;
        }
        return false;
    }

    bool readSchema() {
        std::vector<std::string> sqls[2];   // unused marker to keep the lambda simple
        (void)sqls;
        std::string tilesSql;
        struct Idx { uint32_t root; std::string name, sql; };
        std::vector<Idx> idx;
        uint32_t metaRoot = 0;
        const bool ok = walkTable(1, [&](int64_t, const std::vector<uint8_t>& r) {
            std::vector<Col> c;
            if (!record(r, c) || c.size() < 5) return true;
            const std::string& type = c[0].s; const std::string& name = c[1].s; const std::string& tbl = c[2].s;
            const uint32_t root = (uint32_t)c[3].i;
            if (type == "table" && name == "tiles") { tilesRoot_ = root; tilesSql = c[4].s; }
            else if (type == "table" && name == "metadata") metaRoot = root;
            else if (type == "index" && tbl == "tiles") idx.push_back({root, name, c[4].s});
            return true;
        });
        if (!ok || !tilesRoot_) return false;
        // The column order of the table must be the standard one for the row decoding below
        std::string ts;
        if (!filesp::tilesSchemaOk(tilesSql, ts)) return false;
        for (const Idx& i : idx) {
            // an index on (zoom_level, tile_column, tile_row), or the automatic index of a PRIMARY KEY / UNIQUE constraint of the table
            if (filesp::indexCovers(i.sql, ts)) { indexRoot_ = i.root; break; }
        }
        if (metaRoot) {
            walkTable(metaRoot, [&](int64_t, const std::vector<uint8_t>& r) {
                std::vector<Col> c;
                if (record(r, c) && c.size() >= 2 && !c[0].isNull) meta_.push_back({c[0].s, c[1].isInt ? std::to_string(c[1].i) : c[1].s});
                return meta_.size() < 200;
            });
        }
        return true;
    }

    static int cmp3(int64_t a0, int64_t a1, int64_t a2, int64_t b0, int64_t b1, int64_t b2) {
        if (a0 != b0) return a0 < b0 ? -1 : 1;
        if (a1 != b1) return a1 < b1 ? -1 : 1;
        if (a2 != b2) return a2 < b2 ? -1 : 1;
        return 0;
    }

    // The index b-tree: entries are (zoom_level, tile_column, tile_row, rowid)
    bool indexFind(int64_t z, int64_t x, int64_t row, int64_t& rowid) {
        uint32_t no = indexRoot_;
        for (int depth = 0; depth < 20; depth++) {
            std::vector<uint8_t> pg;
            if (!page(no, pg)) return false;
            uint8_t type; unsigned cells; uint32_t rightMost;
            if (!filesp::pageHeader(pg, 0, type, cells, rightMost)) return false;
            const bool leaf = type == 10;
            if (type != 10 && type != 2) return false;
            const int kind = leaf ? filesp::kIndexLeaf : filesp::kIndexInterior;
            uint32_t next = leaf ? 0 : rightMost;   // right-most child unless a cell says otherwise
            // binary search over the cells
            int lo = 0, hi = (int)cells - 1;
            while (lo <= hi) {
                const int mid = (lo + hi) / 2;
                filesp::CellInfo ci;
                if (!filesp::cell(pg, kind, 0, (size_t)mid, cells, ci)) return false;
                std::vector<uint8_t> rec;
                if (!payload(pg, ci.payloadOff, ci.total, true, rec)) return false;
                std::vector<Col> c;
                if (!record(rec, c, false) || c.size() < 4 || !c[0].isInt || !c[1].isInt || !c[2].isInt || !c[3].isInt) return false;
                const int k = cmp3(z, x, row, c[0].i, c[1].i, c[2].i);
                if (k == 0) { rowid = c[3].i; return true; }   // interior cells carry entries too
                if (k < 0) { hi = mid - 1; next = ci.child; }
                else lo = mid + 1;
            }
            if (leaf) return false;
            // the child to enter: the left child of the first cell greater than the key, else the right-most
            if (lo < (int)cells) {
                filesp::CellInfo ci;
                // an interior index cell is (child pointer, payload): only the pointer is needed here, so the table interior decoder (pointer inside the page) serves
                if (!filesp::cell(pg, filesp::kTableInterior, 0, (size_t)lo, cells, ci)) return false;
                next = ci.child;
            } else next = rightMost;
            no = next;
        }
        return false;
    }

    bool rowToTile(const std::vector<uint8_t>& rec, std::vector<uint8_t>& out) {
        std::vector<Col> c;
        if (!record(rec, c) || c.size() < 4 || !c[3].isBlob) return false;
        out = std::move(c[3].b);
        return !out.empty();
    }

    // The table b-tree by rowid
    bool tableRowTile(int64_t rowid, std::vector<uint8_t>& out) {
        uint32_t no = tilesRoot_;
        for (int depth = 0; depth < 20; depth++) {
            std::vector<uint8_t> pg;
            if (!page(no, pg)) return false;
            uint8_t type; unsigned cells; uint32_t rightMost;
            if (!filesp::pageHeader(pg, 0, type, cells, rightMost)) return false;
            if (type == 5) {
                uint32_t next = rightMost;
                for (unsigned i = 0; i < cells; i++) {
                    filesp::CellInfo c;
                    if (!filesp::cell(pg, filesp::kTableInteriorKey, 0, i, cells, c)) return false;
                    if ((int64_t)c.key >= rowid) { next = c.child; break; }
                }
                no = next;
                continue;
            }
            if (type != 13) return false;
            int lo = 0, hi = (int)cells - 1;
            while (lo <= hi) {
                const int mid = (lo + hi) / 2;
                filesp::CellInfo c;
                if (!filesp::cell(pg, filesp::kTableLeaf, 0, (size_t)mid, cells, c)) return false;
                if ((int64_t)c.key == rowid) {
                    std::vector<uint8_t> rec;
                    return payload(pg, c.payloadOff, c.total, false, rec) && rowToTile(rec, out);
                }
                if ((int64_t)c.key < rowid) lo = mid + 1; else hi = mid - 1;
            }
            return false;
        }
        return false;
    }

    bool scanTile(int64_t z, int64_t x, int64_t row, std::vector<uint8_t>& out) {
        bool found = false;
        walkTable(tilesRoot_, [&](int64_t, const std::vector<uint8_t>& r) {
            std::vector<Col> c;
            if (!record(r, c, false) || c.size() < 4 || !c[0].isInt || !c[1].isInt || !c[2].isInt) return true;
            if (c[0].i == z && c[1].i == x && c[2].i == row) { found = rowToTile(r, out); return false; }
            return true;
        });
        return found;
    }
};

}
