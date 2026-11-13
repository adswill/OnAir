// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Teletext page renderer (ETSI EN 300 706 section 12 and 15): lays a page of 25 rows of 40 characters out as character cells with the spacing attributes
//! applied (colours, alpha and separated mosaics, flash, conceal, hold mosaic, double height, start / end box), and turns a row of cells into UTF-8.
use crate::teletext::g0;

/// One character cell of a rendered page.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Cell {
    /// Unicode character when not a mosaic.
    pub ch: u32,
    /// Mosaic bits: 0 top left, 1 top right, 2 middle left, 3 middle right, 4 bottom left, 5 bottom right.
    pub sextants: u8,
    /// Foreground colour 0..7 (black red green yellow blue magenta cyan white).
    pub fg: u8,
    /// Background colour 0..7.
    pub bg: u8,
    /// 1 top half of a double-height character, 2 bottom half.
    pub dh: u8,
    /// A mosaic cell.
    pub mosaic: u8,
    /// Separated mosaic.
    pub separated: u8,
    /// Flashing.
    pub flash: u8,
    /// Hidden until revealed.
    pub conceal: u8,
    /// Inside a start box / end box pair (true everywhere on a page with no box codes).
    pub boxed: u8,
}

impl Cell {
    /// A blank cell (white on black).
    pub const fn blank() -> Cell {
        Cell { ch: 0x20, sextants: 0, fg: 7, bg: 0, dh: 0, mosaic: 0, separated: 0, flash: 0, conceal: 0, boxed: 0 }
    }
}

/// Rows and columns of a teletext page.
pub const ROWS: usize = 25;
/// Columns.
pub const COLS: usize = 40;

/// Render a page: `rows` are the 25 x 40 characters (7 bit), `row_mask` has bit r set for the rows received, `national` is the national option subset.
pub fn render(rows: &[u8; ROWS * COLS], row_mask: u32, national: i32, g: &mut [Cell; ROWS * COLS]) {
    for c in g.iter_mut() {
        *c = Cell::blank();
    }
    let (mut skip_next, mut any_box) = (false, false);
    for r in 0..ROWS {
        if skip_next {
            skip_next = false;
            continue;
        }
        if row_mask & (1 << r) == 0 {
            continue;
        }
        let (mut fg, mut bg) = (7u8, 0u8);
        let (mut mosaic, mut sep, mut dbl, mut flash, mut hold, mut conceal, mut in_box) =
            (false, false, false, false, false, false, false);
        let mut held = 0u8;
        let mut row_double = false;
        for c in 0..COLS {
            let code = rows[r * COLS + c] & 0x7F;
            let mut cell = Cell::blank();
            if r == 0 && c < 8 {
                g[r * COLS + c] = cell;
                continue;
            }
            if code < 0x20 {
                let mut box_code = false;
                match code {
                    0x00..=0x07 => {
                        fg = code;
                        mosaic = false;
                        conceal = false;
                    }
                    0x08 => flash = true,
                    0x09 => flash = false,
                    0x0A => {
                        in_box = false;
                        box_code = true;
                    }
                    0x0B => {
                        in_box = true;
                        any_box = true;
                        box_code = true;
                    }
                    0x0C => dbl = false,
                    0x0D | 0x0F => {
                        dbl = true;
                        row_double = true;
                    }
                    0x10..=0x17 => {
                        fg = code & 7;
                        mosaic = true;
                        conceal = false;
                    }
                    0x18 => conceal = true,
                    0x19 => sep = false,
                    0x1A => sep = true,
                    0x1C => bg = 0,
                    0x1D => bg = fg,
                    0x1E => hold = true,
                    0x1F => hold = false,
                    _ => {}
                }
                cell.fg = fg;
                cell.bg = bg;
                cell.flash = flash as u8;
                cell.dh = dbl as u8;
                cell.conceal = conceal as u8;
                cell.boxed = (!box_code && in_box) as u8;
                if hold && mosaic && held != 0 {
                    cell.mosaic = 1;
                    cell.sextants = held;
                    cell.separated = sep as u8;
                } else {
                    cell.ch = 0x20;
                }
                g[r * COLS + c] = cell;
                continue;
            }
            cell.fg = fg;
            cell.bg = bg;
            cell.flash = flash as u8;
            cell.dh = dbl as u8;
            cell.conceal = conceal as u8;
            cell.boxed = in_box as u8;
            if mosaic && !(0x40..0x60).contains(&code) {
                cell.mosaic = 1;
                cell.separated = sep as u8;
                cell.sextants = (code & 0x1F) | if code & 0x40 != 0 { 0x20 } else { 0 };
                held = cell.sextants;
            } else {
                cell.ch = g0(code, national);
            }
            g[r * COLS + c] = cell;
        }
        if row_double && r < 24 {
            for c in 0..COLS {
                let mut lo = g[r * COLS + c];
                if lo.dh == 1 {
                    lo.dh = 2;
                } else {
                    let bx = lo.boxed;
                    let src = g[r * COLS + c];
                    lo = Cell::blank();
                    lo.bg = src.bg;
                    lo.fg = src.fg;
                    lo.ch = 0x20;
                    lo.boxed = bx;
                }
                g[(r + 1) * COLS + c] = lo;
            }
            skip_next = true;
        }
    }
    if !any_box {
        for c in g.iter_mut() {
            c.boxed = 1;
        }
    }
}

/// The characters of one row as UTF-8 (mosaics and hidden cells as spaces); `out` needs 120 bytes. Returns the length (0 for a row outside 0..24).
pub fn row_text(g: &[Cell; ROWS * COLS], row: i32, out: &mut [u8]) -> usize {
    if !(0..ROWS as i32).contains(&row) {
        return 0;
    }
    let mut w = 0;
    let mut put = |b: u8| {
        if w < out.len() {
            out[w] = b;
            w += 1;
        }
    };
    for c in &g[row as usize * COLS..(row as usize + 1) * COLS] {
        let u = if c.mosaic != 0 || c.conceal != 0 { 0x20 } else { c.ch };
        if u < 0x80 {
            put(u as u8);
        } else if u < 0x800 {
            put((0xC0 | (u >> 6)) as u8);
            put((0x80 | (u & 0x3F)) as u8);
        } else {
            put((0xE0 | (u >> 12)) as u8);
            put((0x80 | ((u >> 6) & 0x3F)) as u8);
            put((0x80 | (u & 0x3F)) as u8);
        }
    }
    w
}
