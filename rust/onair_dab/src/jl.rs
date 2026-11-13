// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Journaline framing and the packet mode layers below it (ETSI TS 102 979 clause 8, ETSI EN 300 401 clause 5.3.2 for DAB packet mode, ETSI ES 201 980 clause
//! 6.6 for DRM packet mode): the restricted MSC data group check, the TOC management block, the user application data of FIG 0/13 and the header fields of one
//! packet. The JML object parser (it inflates) and the data unit reassembly stay in C++.
use crate::crc::{crc16, tail16};

/// Largest data group field Journaline allows.
pub const MAX_FIELD: usize = 4092;

/// Result of the Journaline data group check.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct JlGroup {
    /// 0 accepted, 1 rejected, 2 rejected for a wrong CRC.
    pub status: u8,
    /// Data group type (0 JML object, 6 management data).
    pub typ: i32,
    /// Length of the field (it starts at byte 2 of the group).
    pub field_len: u32,
}

/// Check one MSC data group as Journaline restricts it: 2 byte header with extension, segmentation and user access flags clear and the CRC flag set, CRC,
/// field of at most 4092 bytes.
pub fn data_group(d: &[u8]) -> JlGroup {
    let n = d.len();
    let mut g = JlGroup { status: 1, ..Default::default() };
    if n < 4 || (d[0] & 0xF0) != 0x40 {
        return g;
    }
    let t = (d[0] & 0x0F) as i32;
    if t != 0 && t != 6 {
        return g;
    }
    if n - 4 > MAX_FIELD {
        return g;
    }
    if crc16(&d[..n - 2]) != tail16(d) {
        g.status = 2;
        return g;
    }
    g.status = 0;
    g.typ = t;
    g.field_len = (n - 4) as u32;
    g
}

/// One TOC entry: object id and description byte.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct TocEntry {
    /// Object id.
    pub id: u32,
    /// Object description.
    pub desc: u32,
}

/// The header of a TOC block.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct TocInfo {
    /// TOC revision.
    pub revision: i32,
    /// Number of blocks.
    pub block_count: i32,
    /// This block's index.
    pub block_index: i32,
    /// Number of objects in the service.
    pub object_count: i32,
    /// Timeout in minutes.
    pub timeout_min: i32,
    /// Entries written (at most `out.len()`, the reference keeps 2048).
    pub n: u32,
}

/// Parse a TOC management block (type byte 0x54 first). `None` when it is malformed or has no entries.
pub fn parse_toc(d: &[u8], out: &mut [TocEntry]) -> Option<TocInfo> {
    let n = d.len();
    if n < 13 || d[0] != 0x54 {
        return None;
    }
    let be = |i: usize| ((d[i] as u32) << 8) | d[i + 1] as u32;
    let mut t = TocInfo {
        revision: d[1] as i32,
        block_count: d[2] as i32,
        block_index: d[3] as i32,
        object_count: be(6) as i32,
        timeout_min: be(8) as i32,
        n: 0,
    };
    let ent_len = d[10] as usize;
    let ext_len = be(11) as usize;
    if ent_len < 3 || t.block_count < 1 || t.block_index >= t.block_count {
        return None;
    }
    let mut p = 13 + ext_len;
    let mut c = 0usize;
    while p + ent_len <= n && c < 2048 {
        if c < out.len() {
            out[c] = TocEntry { id: be(p), desc: d[p + 2] as u32 };
        }
        c += 1;
        p += ent_len;
    }
    t.n = c.min(out.len()) as u32;
    (c > 0).then_some(t)
}

/// User application data of FIG 0/13 (clause 8.1.2): version, extended header length and point of entry (-1 = default root).
pub fn user_app_data(d: &[u8]) -> Option<(i32, i32, i32)> {
    if d.len() < 2 {
        return None;
    }
    let entry = if d.len() >= 4 { ((d[2] as i32) << 8) | d[3] as i32 } else { -1 };
    Some((d[0] as i32, d[1] as i32, entry))
}

/// The header fields of one DAB packet mode packet (EN 300 401 clause 5.3.2.1).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct DabPacket {
    /// The CRC-16 is right.
    pub crc_ok: u8,
    /// First packet of a data group.
    pub first: u8,
    /// Last packet of a data group.
    pub last: u8,
    /// Command packet.
    pub cmd: u8,
    /// Continuity index.
    pub ci: i32,
    /// Packet address.
    pub addr: i32,
    /// Useful data length.
    pub ul: u32,
}

/// Parse a DAB packet (24, 48, 72 or 96 bytes; at least 5).
pub fn dab_packet(p: &[u8]) -> DabPacket {
    let len = p.len();
    if len < 5 {
        return DabPacket::default();
    }
    DabPacket {
        crc_ok: (crc16(&p[..len - 2]) == tail16(p)) as u8,
        ci: ((p[0] >> 4) & 3) as i32,
        first: (p[0] & 8 != 0) as u8,
        last: (p[0] & 4 != 0) as u8,
        addr: (((p[0] & 3) as i32) << 8) | p[1] as i32,
        cmd: (p[2] & 0x80 != 0) as u8,
        ul: (p[2] & 0x7F) as u32,
    }
}

/// The header fields of one DRM packet mode packet (ES 201 980 clause 6.6.1).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct DrmPacket {
    /// The CRC-16 is right.
    pub crc_ok: u8,
    /// First packet of a data unit.
    pub first: u8,
    /// Last packet of a data unit.
    pub last: u8,
    /// The useful length field does not fit the packet.
    pub len_bad: u8,
    /// Packet Id (2 bits).
    pub id: i32,
    /// Continuity index (3 bits).
    pub ci: i32,
    /// Useful data bytes.
    pub n: u32,
    /// Offset of the data bytes.
    pub at: u32,
}

/// Parse a DRM packet of `p.len()` bytes (header byte, data field, CRC-16; at least 4).
pub fn drm_packet(p: &[u8]) -> DrmPacket {
    let sz = p.len();
    if sz < 4 {
        return DrmPacket::default();
    }
    let mut r = DrmPacket {
        crc_ok: (crc16(&p[..sz - 2]) == tail16(p)) as u8,
        first: (p[0] & 0x80 != 0) as u8,
        last: (p[0] & 0x40 != 0) as u8,
        id: ((p[0] >> 4) & 3) as i32,
        ci: (p[0] & 7) as i32,
        n: (sz - 3) as u32,
        at: 1,
        len_bad: 0,
    };
    if p[0] & 0x08 != 0 {
        r.n = p[1] as u32;
        r.at = 2;
        if r.n as usize > sz - 4 {
            r.len_bad = 1;
        }
    }
    r
}
