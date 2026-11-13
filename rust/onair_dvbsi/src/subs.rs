// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! DVB subtitle stream parsing (ETSI EN 300 743 section 7): the PES header of a subtitle PES packet (private stream 1, data_identifier 0x20, PTS), the
//! segment walk, and the parsers of the page composition, region composition, CLUT definition, object data and display definition segments. They return
//! plain records; which page a segment belongs to and what it changes (regions, CLUTs, objects, display queue) stays in C++.
use crate::subpix::ycrcb;

/// The parts of a subtitle PES packet the decoder needs.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct PesInfo {
    /// First byte of the segments (after data_identifier and subtitle_stream_id).
    pub payload_off: u32,
    /// A PTS is present.
    pub has_pts: u8,
    /// PTS in seconds (ticks / 90000).
    pub pts: f64,
}

/// Parse the PES header of a DVB subtitle packet; `None` when it is not private stream 1 with data_identifier 0x20 or too short.
pub fn pes_info(pes: &[u8]) -> Option<PesInfo> {
    let n = pes.len();
    if n < 10 || pes[0] != 0 || pes[1] != 0 || pes[2] != 1 || pes[3] != 0xBD {
        return None;
    }
    let hdr = 9 + pes[8] as usize;
    if hdr + 2 > n {
        return None;
    }
    let mut info = PesInfo { payload_off: (hdr + 2) as u32, has_pts: 0, pts: -1.0 };
    if (pes[7] & 0x80) != 0 && pes[8] >= 5 {
        let q = &pes[9..14];
        let t =
            (((q[0] >> 1) & 7) as u64) << 30 | (q[1] as u64) << 22 | ((q[2] >> 1) as u64) << 15 | (q[3] as u64) << 7 | (q[4] >> 1) as u64;
        info.has_pts = 1;
        info.pts = t as f64 / 90000.0;
    }
    if pes[hdr] != 0x20 {
        return None;
    }
    Some(info)
}

/// One segment of a subtitle PES payload.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Segment {
    /// segment_type.
    pub typ: i32,
    /// page_id.
    pub page: i32,
    /// Offset of the segment data in the payload.
    pub off: u32,
    /// Length of the segment data.
    pub len: u32,
}

/// Walk the segments of a PES payload (sync byte 0x0F, 6 byte header): returns the number found (those past `out.len()` are counted, not stored). A
/// segment that runs past the end stops the walk.
pub fn segments(d: &[u8], out: &mut [Segment]) -> usize {
    let n = d.len();
    let (mut i, mut count) = (0usize, 0usize);
    while i + 6 <= n && d[i] == 0x0F {
        let len = ((d[i + 4] as usize) << 8) | d[i + 5] as usize;
        if i + 6 + len > n {
            break;
        }
        if count < out.len() {
            out[count] =
                Segment { typ: d[i + 1] as i32, page: ((d[i + 2] as i32) << 8) | d[i + 3] as i32, off: (i + 6) as u32, len: len as u32 };
        }
        count += 1;
        i += 6 + len;
    }
    count
}

/// A region listed in a page composition segment.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct PageRegion {
    /// region_id.
    pub id: i32,
    /// Horizontal address.
    pub x: i32,
    /// Vertical address.
    pub y: i32,
}

/// Page composition segment header.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct PageInfo {
    /// page_time_out in seconds.
    pub timeout: i32,
    /// page_version_number.
    pub version: i32,
    /// page_state (0 normal, 1 acquisition point, 2 mode change).
    pub state: i32,
    /// Regions found (those past the array are counted, not stored).
    pub n_regions: u32,
}

/// Parse a page composition segment (`d.len() >= 2`).
pub fn page_composition(d: &[u8], out: &mut [PageRegion]) -> Option<PageInfo> {
    let n = d.len();
    if n < 2 {
        return None;
    }
    let mut info = PageInfo { timeout: d[0] as i32, version: (d[1] >> 4) as i32, state: ((d[1] >> 2) & 3) as i32, n_regions: 0 };
    let mut i = 2;
    let mut c = 0usize;
    while i + 6 <= n {
        if c < out.len() {
            out[c] = PageRegion {
                id: d[i] as i32,
                x: ((d[i + 2] as i32) << 8) | d[i + 3] as i32,
                y: ((d[i + 4] as i32) << 8) | d[i + 5] as i32,
            };
        }
        c += 1;
        i += 6;
    }
    info.n_regions = c as u32;
    Some(info)
}

/// An object placed in a region.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct RegionObject {
    /// object_id.
    pub id: i32,
    /// object_type (0 bitmap, 1 / 2 character coded).
    pub typ: i32,
    /// Horizontal position.
    pub x: i32,
    /// Vertical position.
    pub y: i32,
    /// foreground_pixel_code (character coded objects).
    pub fg: i32,
    /// background_pixel_code.
    pub bg: i32,
}

/// Region composition segment header.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct RegionInfo {
    /// region_id.
    pub id: i32,
    /// region_version_number.
    pub version: i32,
    /// region_fill_flag.
    pub fill: u8,
    /// Width in pixels.
    pub w: i32,
    /// Height in pixels.
    pub h: i32,
    /// Depth in bits: 2, 4 or 8.
    pub depth: i32,
    /// CLUT_id.
    pub clut: i32,
    /// 8-bit pixel code of the fill.
    pub pix8: i32,
    /// 4-bit pixel code of the fill.
    pub pix4: i32,
    /// 2-bit pixel code of the fill.
    pub pix2: i32,
    /// Objects found (those past the array are counted, not stored).
    pub n_objs: u32,
}

/// Parse a region composition segment (`d.len() >= 10`).
pub fn region_composition(d: &[u8], out: &mut [RegionObject]) -> Option<RegionInfo> {
    let n = d.len();
    if n < 10 {
        return None;
    }
    let depth = (d[6] >> 2) & 7;
    let mut r = RegionInfo {
        id: d[0] as i32,
        version: (d[1] >> 4) as i32,
        fill: (d[1] >> 3) & 1,
        w: ((d[2] as i32) << 8) | d[3] as i32,
        h: ((d[4] as i32) << 8) | d[5] as i32,
        depth: if depth == 1 {
            2
        } else if depth == 3 {
            8
        } else {
            4
        },
        clut: d[7] as i32,
        pix8: d[8] as i32,
        pix4: (d[9] >> 4) as i32,
        pix2: ((d[9] >> 2) & 3) as i32,
        n_objs: 0,
    };
    let mut i = 10;
    let mut c = 0usize;
    while i + 6 <= n {
        let mut o = RegionObject {
            id: ((d[i] as i32) << 8) | d[i + 1] as i32,
            typ: (d[i + 2] >> 6) as i32,
            x: (((d[i + 2] & 0x0F) as i32) << 8) | d[i + 3] as i32,
            y: (((d[i + 4] & 0x0F) as i32) << 8) | d[i + 5] as i32,
            fg: 0,
            bg: 0,
        };
        i += 6;
        if o.typ == 1 || o.typ == 2 {
            if i + 2 > n {
                break;
            }
            o.fg = d[i] as i32;
            o.bg = d[i + 1] as i32;
            i += 2;
        }
        if c < out.len() {
            out[c] = o;
        }
        c += 1;
    }
    r.n_objs = c as u32;
    Some(r)
}

/// One CLUT entry: the target tables (`flags` bits 0x80 2-bit, 0x40 4-bit, 0x20 8-bit), the entry id and the colour.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct ClutEntry {
    /// CLUT_entry_id.
    pub id: i32,
    /// The entry flags byte (the full-range flag 0x01 is already applied).
    pub flags: i32,
    /// RGBA as `r | g << 8 | b << 16 | a << 24`.
    pub rgba: u32,
}

/// Parse a CLUT definition segment (`d.len() >= 2`): the CLUT id and up to `out.len()` entries (counted in the second value, stored up to the capacity).
pub fn clut_definition(d: &[u8], out: &mut [ClutEntry]) -> Option<(i32, usize)> {
    let n = d.len();
    if n < 2 {
        return None;
    }
    let mut i = 2;
    let mut c = 0usize;
    while i + 2 <= n {
        let (eid, fl) = (d[i] as i32, d[i + 1] as i32);
        let (y, cr, cb, t);
        if fl & 1 != 0 {
            if i + 6 > n {
                break;
            }
            y = d[i + 2] as i32;
            cr = d[i + 3] as i32;
            cb = d[i + 4] as i32;
            t = d[i + 5] as i32;
            i += 6;
        } else {
            if i + 4 > n {
                break;
            }
            let v = ((d[i + 2] as i32) << 8) | d[i + 3] as i32;
            y = (v >> 10) << 2;
            cr = ((v >> 6) & 15) << 4;
            cb = ((v >> 2) & 15) << 4;
            t = (v & 3) * 85;
            i += 4;
        }
        if c < out.len() {
            out[c] = ClutEntry { id: eid, flags: fl, rgba: ycrcb(y, cr, cb, t) };
        }
        c += 1;
    }
    Some((d[0] as i32, c))
}

/// Display definition segment: the display size (720 x 576 when the stated one is outside 1..4096).
pub fn display_definition(d: &[u8]) -> Option<(i32, i32)> {
    if d.len() < 5 {
        return None;
    }
    let w = (((d[1] as i32) << 8) | d[2] as i32) + 1;
    let h = (((d[3] as i32) << 8) | d[4] as i32) + 1;
    if !(1..=4096).contains(&w) || !(1..=4096).contains(&h) {
        return Some((720, 576));
    }
    Some((w, h))
}

/// Object data segment of a bitmap object: id and the byte ranges of the top and bottom field data.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct ObjectInfo {
    /// object_id.
    pub id: i32,
    /// Offset of the top field data.
    pub top_off: u32,
    /// Length of the top field data.
    pub top_len: u32,
    /// Offset of the bottom field data.
    pub bot_off: u32,
    /// Length of the bottom field data.
    pub bot_len: u32,
}

/// Parse an object data segment; `None` for character coded objects (method not 0), a short segment or field lengths that do not fit.
pub fn object_data(d: &[u8]) -> Option<ObjectInfo> {
    let n = d.len();
    if n < 3 {
        return None;
    }
    let method = (d[2] >> 2) & 3;
    if method != 0 || n < 7 {
        return None;
    }
    let top = ((d[3] as usize) << 8) | d[4] as usize;
    let bot = ((d[5] as usize) << 8) | d[6] as usize;
    if 7 + top + bot > n {
        return None;
    }
    Some(ObjectInfo {
        id: ((d[0] as i32) << 8) | d[1] as i32,
        top_off: 7,
        top_len: top as u32,
        bot_off: (7 + top) as u32,
        bot_len: bot as u32,
    })
}
