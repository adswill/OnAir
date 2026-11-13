// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_ts` (core/include/dect2/rs/ts.h). Every export is `onair_ts_*`; pointers come as (pointer, length) pairs and the caller owns all buffers.
//! Record arrays are passed as (pointer, capacity); parsers return the count written, `usize::MAX` (or false) when the section is too short or a pointer is
//! null. Sizes of the record structs are reported by `onair_ts_struct_sizes` so the C++ side can check its copies at start-up.
#![allow(clippy::missing_safety_doc)]

use crate::eit::{parse_eit, EitEvent, EitInfo};
use crate::packet::{packet_header, pat_packet, PacketHeader};
use crate::si::{self, NitInfo, NitLcn, PatEntry, PmtInfo, PmtStream, PmtSub, SdtService, Tdt, Tvct};

/// Input slice (empty for a null pointer or length 0).
unsafe fn rd<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(p, n)
    }
}

/// Output record array (empty for a null pointer or capacity 0).
unsafe fn arr<'a, T>(p: *mut T, n: usize) -> &'a mut [T] {
    if p.is_null() || n == 0 {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(p, n)
    }
}

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_ts_version() -> u32 {
    1
}

/// sizeof of the record structs in the order PacketHeader, PatEntry, PmtStream, PmtSub, PmtInfo, SdtService, NitLcn, NitInfo, Tvct, Tdt, EitEvent, EitInfo.
#[no_mangle]
pub unsafe extern "C" fn onair_ts_struct_sizes(out: *mut usize) {
    if out.is_null() {
        return;
    }
    let v = [
        core::mem::size_of::<PacketHeader>(),
        core::mem::size_of::<PatEntry>(),
        core::mem::size_of::<PmtStream>(),
        core::mem::size_of::<PmtSub>(),
        core::mem::size_of::<PmtInfo>(),
        core::mem::size_of::<SdtService>(),
        core::mem::size_of::<NitLcn>(),
        core::mem::size_of::<NitInfo>(),
        core::mem::size_of::<Tvct>(),
        core::mem::size_of::<Tdt>(),
        core::mem::size_of::<EitEvent>(),
        core::mem::size_of::<EitInfo>(),
    ];
    for (i, x) in v.iter().enumerate() {
        *out.add(i) = *x;
    }
}

/// MPEG-2 section CRC-32 of `n` bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_ts_crc32(p: *const u8, n: usize) -> u32 {
    crate::crc::mpeg_crc32(rd(p, n))
}

/// Header of a transport packet (`n` bytes, at least 4); false when the sync byte is not 0x47.
#[no_mangle]
pub unsafe extern "C" fn onair_ts_packet_header(p: *const u8, n: usize, out: *mut PacketHeader) -> bool {
    match packet_header(rd(p, n)) {
        Some(h) if !out.is_null() => {
            *out = h;
            true
        }
        _ => false,
    }
}

/// The 188 byte PAT packet of a single-program stream.
#[no_mangle]
pub unsafe extern "C" fn onair_ts_pat_packet(tsid: i32, sid: i32, pmt_pid: i32, cc: i32, out188: *mut u8) {
    if !out188.is_null() {
        pat_packet(tsid, sid, pmt_pid, cc, &mut *(out188 as *mut [u8; 188]));
    }
}

/// PAT: writes the transport_stream_id and up to `cap` entries; returns the count or `usize::MAX`.
#[no_mangle]
pub unsafe extern "C" fn onair_ts_parse_pat(s: *const u8, n: usize, tsid: *mut i32, out: *mut PatEntry, cap: usize) -> usize {
    match si::parse_pat(rd(s, n), arr(out, cap)) {
        Some((t, c)) if !tsid.is_null() => {
            *tsid = t;
            c
        }
        _ => usize::MAX,
    }
}

/// PMT: writes the info, up to `scap` streams and `bcap` sub entries; false when the section is too short.
#[no_mangle]
pub unsafe extern "C" fn onair_ts_parse_pmt(
    s: *const u8,
    n: usize,
    info: *mut PmtInfo,
    streams: *mut PmtStream,
    scap: usize,
    subs: *mut PmtSub,
    bcap: usize,
) -> bool {
    match si::parse_pmt(rd(s, n), arr(streams, scap), arr(subs, bcap)) {
        Some(i) if !info.is_null() => {
            *info = i;
            true
        }
        _ => false,
    }
}

/// SDT: transport_stream_id, original_network_id and up to `cap` services; returns the count or `usize::MAX`.
#[no_mangle]
pub unsafe extern "C" fn onair_ts_parse_sdt(
    s: *const u8,
    n: usize,
    tsid: *mut i32,
    onid: *mut i32,
    out: *mut SdtService,
    cap: usize,
) -> usize {
    match si::parse_sdt(rd(s, n), arr(out, cap)) {
        Some((t, o, c)) if !tsid.is_null() && !onid.is_null() => {
            *tsid = t;
            *onid = o;
            c
        }
        _ => usize::MAX,
    }
}

/// NIT: network name and up to `cap` logical channel numbers; false when the section is too short.
#[no_mangle]
pub unsafe extern "C" fn onair_ts_parse_nit(s: *const u8, n: usize, info: *mut NitInfo, out: *mut NitLcn, cap: usize) -> bool {
    match si::parse_nit(rd(s, n), arr(out, cap)) {
        Some(i) if !info.is_null() => {
            *info = i;
            true
        }
        _ => false,
    }
}

/// ATSC TVCT / CVCT: transport_stream_id and up to `cap` channels; returns the count or `usize::MAX`.
#[no_mangle]
pub unsafe extern "C" fn onair_ts_parse_tvct(s: *const u8, n: usize, tsid: *mut i32, out: *mut Tvct, cap: usize) -> usize {
    match si::parse_tvct(rd(s, n), arr(out, cap)) {
        Some((t, c)) if !tsid.is_null() => {
            *tsid = t;
            c
        }
        _ => usize::MAX,
    }
}

/// TDT / TOT time; false when the section is shorter than 8 bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_ts_parse_tdt(s: *const u8, n: usize, out: *mut Tdt) -> bool {
    match si::parse_tdt(rd(s, n)) {
        Some(t) if !out.is_null() => {
            *out = t;
            true
        }
        _ => false,
    }
}

/// EIT: header info and up to `cap` events; false when the section is shorter than 18 bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_ts_parse_eit(s: *const u8, n: usize, info: *mut EitInfo, out: *mut EitEvent, cap: usize) -> bool {
    match parse_eit(rd(s, n), arr(out, cap)) {
        Some(i) if !info.is_null() => {
            *info = i;
            true
        }
        _ => false,
    }
}
