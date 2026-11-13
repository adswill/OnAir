// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The plain `repr(C)` records the DAB parsers fill in (safe code: the C ABI that passes them is in `ffi.rs`). The field names follow the C header
//! core/include/dect2/rs/dab.h. All numbers are `i32` / `u32` unless a smaller type says it is a flag.

/// One FIG of a FIB: type and the byte range of its data field (header byte first) in the FIB.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct FigRef {
    /// FIG type 0..6.
    pub typ: i32,
    /// First byte of the data field.
    pub off: u32,
    /// Length of the data field.
    pub len: u32,
}

/// FIG header flags: C/N, other ensemble, programme / data, extension.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct FigHead {
    /// C/N.
    pub cn: i32,
    /// OE.
    pub oe: i32,
    /// P/D.
    pub pd: i32,
    /// Extension.
    pub ext: i32,
}

/// FIG 0/1 sub-channel entry as sent (no derived size / bit rate).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Fig01Sub {
    /// SubChId.
    pub id: i32,
    /// Start address (CU).
    pub start: i32,
    /// Equal error protection (long form).
    pub eep: u8,
    /// UEP: table switch bit set (table 1, not defined).
    pub uep_table: u8,
    /// EEP option (0 = A, 1 = B profiles).
    pub option: i32,
    /// EEP protection level 0..3.
    pub level: i32,
    /// EEP size in CUs.
    pub size: i32,
    /// UEP table index.
    pub uep_index: i32,
}

/// FIG 0/3 packet mode component.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Fig03Packet {
    /// SCId.
    pub scid: i32,
    /// Data groups are used (the DG flag is inverted on the air).
    pub dg: u8,
    /// DSCTy.
    pub dscty: i32,
    /// SubChId.
    pub sub_id: i32,
    /// Packet address.
    pub address: i32,
}

/// FIG 0/2 service with its components in the component array.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Fig02Service {
    /// 16 or 32 bit service identifier.
    pub sid: u32,
    /// Index of its first component.
    pub first: u32,
    /// Number of components.
    pub ncomp: u32,
}

/// FIG 0/2 service component; fields that do not apply to the transport mechanism are -1 (`primary` 1).
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Fig02Comp {
    /// TMId: 0 stream audio, 1 stream data, 3 packet data.
    pub tmid: i32,
    /// ASCTy (audio).
    pub ascty: i32,
    /// DSCTy (stream data).
    pub dscty: i32,
    /// SubChId.
    pub sub_id: i32,
    /// SCId (packet data).
    pub scid: i32,
    /// Primary component.
    pub primary: u8,
}

impl Default for Fig02Comp {
    fn default() -> Self {
        Fig02Comp { tmid: 0, ascty: -1, dscty: -1, sub_id: -1, scid: -1, primary: 1 }
    }
}

/// FIG 0/13 user application of a service.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Fig013App {
    /// Service identifier.
    pub sid: u32,
    /// SCIdS.
    pub scids: i32,
    /// Index in the service's application list.
    pub index: i32,
    /// User application type (11 bits).
    pub typ: i32,
    /// First byte of the user application data in the FIG data field.
    pub off: u32,
    /// Length of the user application data.
    pub len: u32,
}

/// FIG 0/17 programme type entry (`lang`, `pty` -1 when not sent).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Fig017Pty {
    /// Service identifier.
    pub sid: u32,
    /// S/D flag (dynamic).
    pub sd: u8,
    /// Primary (P/S flag 0).
    pub primary: u8,
    /// Language, -1 none.
    pub lang: i32,
    /// Programme type, -1 none.
    pub pty: i32,
}

/// FIG 0/18 announcement support entry; its clusters are `clusters[first..first + ncl]`.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Fig018Support {
    /// Service identifier.
    pub sid: u32,
    /// ASu flags.
    pub asu: u32,
    /// Index of its first cluster.
    pub first: u32,
    /// Number of clusters.
    pub ncl: u32,
}

/// FIG 0/19 announcement switching entry.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Fig019Switch {
    /// Cluster id.
    pub cluster: i32,
    /// ASw flags.
    pub asw: u32,
    /// New flag.
    pub new_flag: u8,
    /// Region flag.
    pub region: u8,
    /// Sub-channel carrying the announcement.
    pub sub_id: i32,
}

/// FIG 0/10 date and time.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Fig010Time {
    /// Modified Julian Date.
    pub mjd: u32,
    /// Hours.
    pub hour: i32,
    /// Minutes.
    pub minute: i32,
    /// Seconds (0 when the short form is sent).
    pub second: i32,
    /// Seconds since 1970, 0 when the MJD is not after 1970-01-01.
    pub utc: i64,
}

/// FIG 1 label: the entity (extension 0 ensemble, 1 programme service, 4 service component, 5 data service), its ids and the 16 label bytes.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Fig1Label {
    /// Extension.
    pub ext: i32,
    /// Character set field.
    pub charset: i32,
    /// SCIdS (extension 4).
    pub scids: i32,
    /// Service identifier.
    pub sid: u32,
    /// First byte of the 16 label bytes in the FIG data field.
    pub off: u32,
}

/// FIG 2 segment: the identity of the extended label and the byte range of the text in this segment.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Fig2Seg {
    /// Extension (0, 1, 4, 5).
    pub ext: i32,
    /// Toggle bit.
    pub toggle: i32,
    /// Segment index.
    pub seg: i32,
    /// Rfu / text control bit.
    pub rfu: i32,
    /// SCIdS (extension 4).
    pub scids: i32,
    /// Service identifier.
    pub sid: u32,
    /// Segment 0: the text is UCS-2.
    pub ucs2: u8,
    /// Segment 0: total number of segments.
    pub total: i32,
    /// The segment carries text (false: segment 0 with no text after its header fields).
    pub valid: u8,
    /// First text byte in the FIG data field.
    pub text_off: u32,
    /// Number of text bytes.
    pub text_len: u32,
}
