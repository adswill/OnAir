// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The `repr(C)` structs of the DMR C ABI (mirrored by core/include/dect2/rs/dmr.h) and their conversions to and from the safe types of the crate.
use crate::data::DataHeader;
use crate::lc::FullLc;
use crate::text::TalkerAlias;

/// A full link control PDU.
#[repr(C)]
pub struct OnairDmrFullLc {
    pub pf: u8,
    pub flco: i32,
    pub fid: i32,
    pub svc: u8,
    pub dst: u32,
    pub src: u32,
    pub raw: [u8; 9],
}
/// A control signalling block.
#[repr(C)]
pub struct OnairDmrCsbk {
    pub lb: u8,
    pub pf: u8,
    pub opcode: i32,
    pub fid: i32,
    pub data: [u8; 8],
}
/// A data header.
#[repr(C)]
pub struct OnairDmrDataHeader {
    pub dpf: i32,
    pub sap: i32,
    pub group: u8,
    pub resp: u8,
    pub dst: u32,
    pub src: u32,
    pub blocks: i32,
    pub pad: i32,
    pub fmf: u8,
    pub fsn: i32,
    pub ns: i32,
    pub dd: i32,
    pub sp: i32,
    pub dp: i32,
    pub resync: u8,
    pub sarq: u8,
    pub status: i32,
}
/// A parsed data block (`len` data octets in `bytes`).
#[repr(C)]
pub struct OnairDmrDataBlock {
    pub bytes: [u8; 24],
    pub len: u32,
    pub dbsn: u32,
    pub crc_ok: u8,
}
/// A talker alias in assembly.
#[repr(C)]
pub struct OnairDmrTa {
    pub format: i32,
    pub length: i32,
    pub have: u8,
    pub data: [u8; 28],
}

pub fn lc_to_c(l: &FullLc) -> OnairDmrFullLc {
    OnairDmrFullLc { pf: l.pf as u8, flco: l.flco, fid: l.fid, svc: l.svc, dst: l.dst, src: l.src, raw: l.raw }
}
pub fn lc_from_c(l: &OnairDmrFullLc) -> FullLc {
    FullLc { pf: l.pf != 0, flco: l.flco, fid: l.fid, svc: l.svc, dst: l.dst, src: l.src, raw: l.raw }
}
pub fn dh_to_c(h: &DataHeader) -> OnairDmrDataHeader {
    OnairDmrDataHeader {
        dpf: h.dpf,
        sap: h.sap,
        group: h.group as u8,
        resp: h.resp as u8,
        dst: h.dst,
        src: h.src,
        blocks: h.blocks,
        pad: h.pad,
        fmf: h.fmf as u8,
        fsn: h.fsn,
        ns: h.ns,
        dd: h.dd,
        sp: h.sp,
        dp: h.dp,
        resync: h.resync as u8,
        sarq: h.sarq as u8,
        status: h.status,
    }
}
pub fn dh_from_c(h: &OnairDmrDataHeader) -> DataHeader {
    DataHeader {
        dpf: h.dpf,
        sap: h.sap,
        group: h.group != 0,
        resp: h.resp != 0,
        dst: h.dst,
        src: h.src,
        blocks: h.blocks,
        pad: h.pad,
        fmf: h.fmf != 0,
        fsn: h.fsn,
        ns: h.ns,
        dd: h.dd,
        sp: h.sp,
        dp: h.dp,
        resync: h.resync != 0,
        sarq: h.sarq != 0,
        status: h.status,
    }
}
pub fn ta_to_c(t: &TalkerAlias) -> OnairDmrTa {
    OnairDmrTa { format: t.format, length: t.length, have: t.have, data: t.data }
}
pub fn ta_from_c(t: &OnairDmrTa) -> TalkerAlias {
    TalkerAlias { format: t.format, length: t.length, have: t.have, data: t.data }
}
