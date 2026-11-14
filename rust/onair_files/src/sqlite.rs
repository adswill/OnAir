// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The SQLite file format pieces behind the MBTiles map reader (https://www.sqlite.org/fileformat2.html): database header, page header, b-tree cells (table leaf
//! 13, table interior 5, index leaf 10, index interior 2), variable-length integers, record decoding, overflow payload sizes, and the text checks of the `tiles`
//! schema. Everything works on page buffers the caller read; the file I/O, the b-tree walks and the overflow chains stay in C++ (`core/include/dect2/geo/mbtiles.h`).
//! Also the tile coordinate rule of MBTiles: rows count from the south (TMS), the app's tiles are XYZ.
use crate::Span;

/// The numbers of the 100 byte database header the reader needs.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct DbHeader {
    /// 0 = ok, 1 = not an SQLite file, 2 = bad page size, 3 = bad page layout, 4 = WAL mode, 5 = text is not UTF-8.
    pub status: u32,
    /// Page size in bytes.
    pub page_size: u32,
    /// Bytes reserved at the end of each page.
    pub reserved: u32,
    /// page_size - reserved.
    pub usable: u32,
}

/// Reads the database header (the first 100 bytes of the file).
pub fn db_header(h: &[u8]) -> DbHeader {
    let mut r = DbHeader::default();
    if h.len() < 100 || &h[..16] != b"SQLite format 3\0" {
        r.status = 1;
        return r;
    }
    let mut ps = ((h[16] as u32) << 8) | h[17] as u32;
    if ps == 1 {
        ps = 65536;
    }
    if !(512..=65536).contains(&ps) || ps & (ps - 1) != 0 {
        r.status = 2;
        return r;
    }
    let reserved = h[20] as u32;
    let usable = ps - reserved;
    if usable < 480 {
        r.status = 3;
        return r;
    }
    if h[18] == 2 || h[19] == 2 {
        r.status = 4;
        return r;
    }
    let enc = u32::from_be_bytes([h[56], h[57], h[58], h[59]]);
    if enc != 0 && enc != 1 {
        r.status = 5;
        return r;
    }
    r.page_size = ps;
    r.reserved = reserved;
    r.usable = usable;
    r
}

/// SQLite variable-length integer (1 to 9 bytes, big endian, 7 bits per byte, the ninth byte whole): the value and the bytes used, None when `p` ends first.
pub fn varint(p: &[u8]) -> Option<(u64, usize)> {
    let mut v: u64 = 0;
    for i in 0..8 {
        let b = *p.get(i)?;
        v = (v << 7) | (b & 0x7F) as u64;
        if b & 0x80 == 0 {
            return Some((v, i + 1));
        }
    }
    let b = *p.get(8)?;
    Some(((v << 8) | b as u64, 9))
}

/// One column of a record.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Col {
    /// 0 NULL, 1 integer, 2 float (not decoded), 3 text, 4 blob.
    pub kind: u32,
    /// Integer value (kind 1).
    pub int: i64,
    /// Text / blob bytes inside the record.
    pub data: Span,
}

/// Decodes a record into `cols`. Returns the number of columns, or None for a damaged record; with `cols` shorter than the record the count is still right.
pub fn record(r: &[u8], cols: &mut [Col]) -> Option<usize> {
    let (hs, n) = varint(r)?;
    if hs > r.len() as u64 || hs < n as u64 {
        onair_common::diag_debug!(515611, "A database record of {} bytes announces a header of {} bytes, so the record is damaged and was skipped.", r.len(), hs);
        return None;
    }
    let hs = hs as usize;
    let mut h = n;
    let mut d = hs;
    let mut count = 0usize;
    while h < hs {
        let (t, m) = varint(&r[h..hs])?;
        h += m;
        let mut c = Col::default();
        let mut len = 0usize;
        match t {
            0 => {}
            1..=6 => {
                const SZ: [usize; 7] = [0, 1, 2, 3, 4, 6, 8];
                len = SZ[t as usize];
                if d + len > r.len() {
                    return None;
                }
                let mut v = r[d] as i8 as i64;
                for &b in &r[d + 1..d + len] {
                    v = (v << 8) | b as i64;
                }
                c.kind = 1;
                c.int = v;
            }
            7 => {
                len = 8;
                if d + len > r.len() {
                    return None;
                }
                c.kind = 2;
            }
            8 => {
                c.kind = 1;
            }
            9 => {
                c.kind = 1;
                c.int = 1;
            }
            12.. => {
                len = ((t - 12) / 2) as usize;
                if d + len > r.len() {
                    return None;
                }
                c.kind = if t & 1 == 1 { 3 } else { 4 };
                c.data = Span { off: d as u32, len: len as u32 };
            }
            _ => return None,
        }
        d += len;
        if count < cols.len() {
            cols[count] = c;
        }
        count += 1;
    }
    Some(count)
}

/// How much of a payload of `total` bytes sits on the b-tree page: `Some((local, overflows))` for `usable` bytes per page, None for a payload over 256 MiB or a
/// page too small to hold a cell. An index b-tree (`index_tree`) keeps less on the page than a table b-tree.
pub fn payload_local(total: u64, usable: u64, index_tree: bool) -> Option<(u64, bool)> {
    if total > (1 << 28) || usable < 36 {
        onair_common::diag_debug!(152343, "A database cell with a payload of {} bytes on pages of {} usable bytes is not valid, so it was skipped.", total, usable);
        return None;
    }
    // unsigned 64 bit arithmetic that wraps, like the reference (a usable size under 480 is rejected when the file is opened; the wraps only matter for odd test values)
    let u = usable;
    let x = if index_tree { (u - 12).wrapping_mul(64) / 255 } else { u - 35 };
    let x = if index_tree { x.wrapping_sub(23) } else { x };
    if total <= x {
        return Some((total, false));
    }
    let m = ((u - 12).wrapping_mul(32) / 255).wrapping_sub(23);
    let k = m.wrapping_add(total.wrapping_sub(m) % (u - 4));
    Some((if k <= x { k } else { m }, true))
}

/// Page kinds the reader walks.
pub const TABLE_LEAF: u8 = 13;
/// Table interior page.
pub const TABLE_INTERIOR: u8 = 5;
/// Not a page type: the same cell as `TABLE_INTERIOR` but also decoding its key (the search by rowid needs it; the full walk does not and accepts a cell that ends the page).
pub const TABLE_INTERIOR_KEY: u8 = 6;
/// Index leaf page.
pub const INDEX_LEAF: u8 = 10;
/// Index interior page.
pub const INDEX_INTERIOR: u8 = 2;

/// The page header at `base` (100 on page 1, else 0): page type, cell count and, for interior pages, the right-most child. None when the page is too short.
pub fn page_header(pg: &[u8], base: usize) -> Option<(u8, u32, u32)> {
    if pg.len() < base + 12 {
        return None;
    }
    let ty = pg[base];
    let cells = u16::from_be_bytes([pg[base + 3], pg[base + 4]]) as u32;
    let right = u32::from_be_bytes([pg[base + 8], pg[base + 9], pg[base + 10], pg[base + 11]]);
    Some((ty, cells, right))
}

/// A decoded b-tree cell.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Cell {
    /// Left child page (interior cells), else 0.
    pub child: u32,
    /// Where the cell's payload starts in the page.
    pub payload_off: u32,
    /// Payload bytes in all (local + overflow); 0 for a table interior cell.
    pub total: u64,
    /// Table leaf: the rowid; table interior: the key (largest rowid of the left child).
    pub key: u64,
}

fn be16(pg: &[u8], i: usize) -> Option<usize> {
    Some(u16::from_be_bytes([*pg.get(i)?, *pg.get(i + 1)?]) as usize)
}

fn be32(pg: &[u8], i: usize) -> Option<u32> {
    Some(u32::from_be_bytes([*pg.get(i)?, *pg.get(i + 1)?, *pg.get(i + 2)?, *pg.get(i + 3)?]))
}

/// Cell `i` of a page of kind `kind` whose header is at `base`: reads the cell pointer, checks it lies inside the page and decodes the cell. None for a damaged cell
/// (pointer or varint outside the page). For the table interior kinds with `i` equal to the cell count the right-most child is returned.
pub fn cell(pg: &[u8], kind: u8, base: usize, i: usize, cells: usize) -> Option<Cell> {
    let hdr = base + if kind == TABLE_LEAF || kind == INDEX_LEAF { 8 } else { 12 };
    let mut c = Cell::default();
    if (kind == TABLE_INTERIOR || kind == TABLE_INTERIOR_KEY) && i == cells {
        c.child = be32(pg, base + 8)?;
        return Some(c);
    }
    let ptr_at = hdr + 2 * i;
    if ptr_at + 2 > pg.len() {
        return None;
    }
    let mut o = be16(pg, ptr_at)?;
    match kind {
        TABLE_LEAF => {
            if o >= pg.len() {
                return None;
            }
            let (total, a) = varint(&pg[o..])?;
            let (rowid, b) = varint(&pg[o + a..])?;
            c.total = total;
            c.key = rowid;
            c.payload_off = (o + a + b) as u32;
        }
        TABLE_INTERIOR => {
            if o + 4 > pg.len() {
                return None;
            }
            c.child = be32(pg, o)?;
        }
        TABLE_INTERIOR_KEY => {
            // child pointer (4) then the key varint
            if o + 5 > pg.len() {
                return None;
            }
            c.child = be32(pg, o)?;
            let (key, _) = varint(&pg[o + 4..])?;
            c.key = key;
        }
        INDEX_LEAF => {
            if o >= pg.len() {
                return None;
            }
            let (total, a) = varint(&pg[o..])?;
            c.total = total;
            c.payload_off = (o + a) as u32;
        }
        INDEX_INTERIOR => {
            if o + 4 > pg.len() {
                return None;
            }
            c.child = be32(pg, o)?;
            o += 4;
            if o >= pg.len() {
                return None;
            }
            let (total, a) = varint(&pg[o..])?;
            c.total = total;
            c.payload_off = (o + a) as u32;
        }
        _ => return None,
    }
    Some(c)
}

/// MBTiles keeps rows from the south: for the XYZ tile (z, x, y) the stored row (2^z - 1 - y), or None for a tile outside the pyramid (z over 30, negative or
/// out of range x / y).
pub fn tile_row(z: i32, x: i32, y: i32) -> Option<i64> {
    if !(0..=30).contains(&z) || x < 0 || y < 0 {
        return None;
    }
    let n = 1i64 << z;
    if x as i64 >= n || y as i64 >= n {
        return None;
    }
    Some(n - 1 - y as i64)
}

/// True when the bytes are a PNG or JPEG picture (what the raster map can draw), not a vector tile.
pub fn is_raster(d: &[u8]) -> bool {
    is_png(d) || (d.len() >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
}

/// True for the PNG signature.
pub fn is_png(d: &[u8]) -> bool {
    d.len() >= 8 && d[0] == 0x89 && d[1] == b'P' && d[2] == b'N' && d[3] == b'G'
}

/// `s` lower-cased with spaces, newlines, tabs, quotes, backticks and brackets removed (SQL text compared without its formatting), into `out`; returns the length.
/// `out` must hold `s.len()` bytes (else the text is cut).
pub fn squash(s: &[u8], out: &mut [u8]) -> usize {
    let mut n = 0;
    for &c in s {
        if matches!(c, b' ' | b'\n' | b'\t' | b'"' | b'`' | b'[' | b']') {
            continue;
        }
        if n < out.len() {
            out[n] = c.to_ascii_lowercase();
            n += 1;
        }
    }
    n
}

fn find(h: &[u8], pat: &[u8]) -> usize {
    if pat.len() <= h.len() {
        for i in 0..=h.len() - pat.len() {
            if &h[i..i + pat.len()] == pat {
                return i;
            }
        }
    }
    usize::MAX
}

/// The column order check of the `tiles` table: the CREATE statement (squashed in `scratch`, `scratch.len() >= sql.len()`) must list zoom_level, tile_column,
/// tile_row, tile_data in that order. Returns the squashed length for the caller's index check, or None when the layout is not the standard one.
pub fn tiles_schema_ok(sql: &[u8], scratch: &mut [u8]) -> Option<usize> {
    let n = squash(sql, scratch);
    let ts = &scratch[..n];
    let lp = ts.iter().position(|&c| c == b'(');
    let std_layout = match lp {
        Some(lp) => ts.len() - (lp + 1) >= 38 && &ts[lp + 1..lp + 39] == b"zoom_level,tile_column,tile_row,tile_d",
        None => false,
    };
    // `string::compare` of a shorter tail is a mismatch, same as here
    if !std_layout {
        let a = find(ts, b"zoom_level");
        let b = find(ts, b"tile_column");
        let c = find(ts, b"tile_row");
        let d = find(ts, b"tile_data");
        if a == usize::MAX || !(a < b && b < c && c < d) {
            return None;
        }
    }
    Some(n)
}

/// Whether an index on `tiles` covers (zoom_level, tile_column, tile_row): its own SQL says so, or it has no SQL (the automatic index of a PRIMARY KEY /
/// UNIQUE constraint) and the table's squashed SQL (`tiles_squashed`) declares it.
pub fn index_covers(idx_sql: &[u8], scratch: &mut [u8], tiles_squashed: &[u8]) -> bool {
    const PAT: &[u8] = b"(zoom_level,tile_column,tile_row)";
    let n = squash(idx_sql, scratch);
    if find(&scratch[..n], PAT) != usize::MAX {
        return true;
    }
    idx_sql.is_empty() && find(tiles_squashed, PAT) != usize::MAX
}
