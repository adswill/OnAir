// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_dab` (core/include/dect2/rs/dab.h). Every export is `onair_dab_*`; pointers come as (pointer, length) pairs and the caller owns all buffers.
//! Record arrays are (pointer, capacity); parsers return the number of entries found, counted past the capacity, `usize::MAX` (or false) when the input is too
//! short or a pointer is null. `onair_dab_struct_sizes` lets the C++ side check the record layouts at start-up.
#![allow(clippy::missing_safety_doc)]

use crate::ffi_types::*;
use crate::jl::{self, DabPacket, DrmPacket, JlGroup, TocEntry, TocInfo};
use crate::mot::{self, DirEntry, MotGroup, MotHeader, MotParam, MotTime};
use crate::pad::{self, DlSegment, DlpTag, DlsStep, PadBytes, PadSub};
use crate::{crc, fig};

/// Input slice (empty for a null pointer or length 0).
unsafe fn rd<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(p, n)
    }
}

/// Output slice (empty for a null pointer or capacity 0).
unsafe fn wr<'a, T>(p: *mut T, n: usize) -> &'a mut [T] {
    if p.is_null() || n == 0 {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(p, n)
    }
}

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_dab_version() -> u32 {
    1
}

/// sizeof of the record structs in the order FigRef, FigHead, Fig01Sub, Fig03Packet, Fig02Service, Fig02Comp, Fig013App, Fig017Pty, Fig018Support, Fig019Switch,
/// Fig010Time, Fig1Label, Fig2Seg, MotTime, MotHeader, MotParam, MotGroup, DirEntry, PadBytes, PadSub, DlsStep, DlSegment, DlpTag, JlGroup, TocEntry, TocInfo,
/// DabPacket, DrmPacket.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_struct_sizes(out: *mut usize) {
    if out.is_null() {
        return;
    }
    use core::mem::size_of as s;
    let v = [
        s::<FigRef>(),
        s::<FigHead>(),
        s::<Fig01Sub>(),
        s::<Fig03Packet>(),
        s::<Fig02Service>(),
        s::<Fig02Comp>(),
        s::<Fig013App>(),
        s::<Fig017Pty>(),
        s::<Fig018Support>(),
        s::<Fig019Switch>(),
        s::<Fig010Time>(),
        s::<Fig1Label>(),
        s::<Fig2Seg>(),
        s::<MotTime>(),
        s::<MotHeader>(),
        s::<MotParam>(),
        s::<MotGroup>(),
        s::<DirEntry>(),
        s::<PadBytes>(),
        s::<PadSub>(),
        s::<DlsStep>(),
        s::<DlSegment>(),
        s::<DlpTag>(),
        s::<JlGroup>(),
        s::<TocEntry>(),
        s::<TocInfo>(),
        s::<DabPacket>(),
        s::<DrmPacket>(),
    ];
    for (i, x) in v.iter().enumerate() {
        *out.add(i) = *x;
    }
}

/// CRC-16 of DAB (CCITT, start 0xFFFF, inverted).
#[no_mangle]
pub unsafe extern "C" fn onair_dab_crc16(p: *const u8, n: usize) -> u16 {
    crc::crc16(rd(p, n))
}

/// The FIGs of a 30 byte FIB data field; returns the count (also past the capacity) and writes the bytes used.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_fib_walk(fib: *const u8, n: usize, out: *mut FigRef, cap: usize, used: *mut u32) -> usize {
    let (c, u) = fig::fib_walk(rd(fib, n), wr(out, cap));
    if !used.is_null() {
        *used = u as u32;
    }
    c
}

/// FIG header flags for a FIG of this type.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_fig_head(typ: i32, d: *const u8, n: usize, out: *mut FigHead) {
    if !out.is_null() {
        *out = fig::fig_head(typ, rd(d, n));
    }
}

/// FIG 0/0 ensemble id; false when the FIG is too short.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_fig0_eid(d: *const u8, n: usize, eid: *mut i32) -> bool {
    match fig::fig0_eid(rd(d, n)) {
        Some(e) if !eid.is_null() => {
            *eid = e;
            true
        }
        _ => false,
    }
}

/// FIG 0/1 sub-channel entries.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_fig0_subchannels(d: *const u8, n: usize, out: *mut Fig01Sub, cap: usize) -> usize {
    fig::fig0_subchannels(rd(d, n), wr(out, cap))
}

/// FIG 0/3 packet mode components.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_fig0_packet(d: *const u8, n: usize, out: *mut Fig03Packet, cap: usize) -> usize {
    fig::fig0_packet(rd(d, n), wr(out, cap))
}

/// FIG 0/2 services and components; returns the service count and writes the component count.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_fig0_services(
    d: *const u8,
    n: usize,
    svcs: *mut Fig02Service,
    scap: usize,
    comps: *mut Fig02Comp,
    ccap: usize,
    ncomp: *mut usize,
) -> usize {
    let (s, c) = fig::fig0_services(rd(d, n), wr(svcs, scap), wr(comps, ccap));
    if !ncomp.is_null() {
        *ncomp = c;
    }
    s
}

/// FIG 0/13 user applications.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_fig0_userapps(d: *const u8, n: usize, out: *mut Fig013App, cap: usize) -> usize {
    fig::fig0_userapps(rd(d, n), wr(out, cap))
}

/// FIG 0/17 programme types.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_fig0_pty(d: *const u8, n: usize, out: *mut Fig017Pty, cap: usize) -> usize {
    fig::fig0_pty(rd(d, n), wr(out, cap))
}

/// FIG 0/18 announcement support; returns the entry count and writes the cluster count.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_fig0_ann_support(
    d: *const u8,
    n: usize,
    out: *mut Fig018Support,
    cap: usize,
    clusters: *mut u8,
    ccap: usize,
    ncl: *mut usize,
) -> usize {
    let (c, k) = fig::fig0_ann_support(rd(d, n), wr(out, cap), wr(clusters, ccap));
    if !ncl.is_null() {
        *ncl = k;
    }
    c
}

/// FIG 0/19 announcement switching.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_fig0_ann_switch(d: *const u8, n: usize, out: *mut Fig019Switch, cap: usize) -> usize {
    fig::fig0_ann_switch(rd(d, n), wr(out, cap))
}

/// FIG 0/10 date and time; false when too short.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_fig0_time(d: *const u8, n: usize, out: *mut Fig010Time) -> bool {
    match fig::fig0_time(rd(d, n)) {
        Some(t) if !out.is_null() => {
            *out = t;
            true
        }
        _ => false,
    }
}

/// FIG 1 label location; false for the labels that are not followed.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_fig1(d: *const u8, n: usize, out: *mut Fig1Label) -> bool {
    match fig::fig1(rd(d, n)) {
        Some(l) if !out.is_null() => {
            *out = l;
            true
        }
        _ => false,
    }
}

/// FIG 2 segment; false when the FIG is not a followed extended label or too short.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_fig2(d: *const u8, n: usize, out: *mut Fig2Seg) -> bool {
    match fig::fig2(rd(d, n)) {
        Some(l) if !out.is_null() => {
            *out = l;
            true
        }
        _ => false,
    }
}

/// MOT time: bytes used (0 when too short); the record is always written.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_mot_parse_time(d: *const u8, n: usize, t: *mut MotTime) -> usize {
    let mut x = MotTime::default();
    let u = mot::parse_time(rd(d, n), &mut x);
    if !t.is_null() {
        *t = x;
    }
    u
}

/// MOT time as text (64 bytes are enough); returns the length.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_mot_time_text(t: *const MotTime, out: *mut u8, cap: usize) -> usize {
    if t.is_null() {
        return 0;
    }
    mot::time_text(&*t, wr(out, cap))
}

/// MOT header: fills the record, up to `pcap` parameters and the cleaned caption (needs 3 bytes per header byte); false when shorter than 7 bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_mot_parse_header(
    h: *const u8,
    n: usize,
    out: *mut MotHeader,
    params: *mut MotParam,
    pcap: usize,
    caption: *mut u8,
    ccap: usize,
) -> bool {
    match mot::parse_header(rd(h, n), wr(params, pcap), wr(caption, ccap)) {
        Some(o) if !out.is_null() => {
            *out = o;
            true
        }
        _ => false,
    }
}

/// MOT content type name (up to 24 bytes); returns the length.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_mot_content_type_name(typ: i32, sub: i32, out: *mut u8, cap: usize) -> usize {
    let s = mot::content_type_name(typ, sub);
    let o = wr(out, cap);
    let k = s.len().min(o.len());
    o[..k].copy_from_slice(&s[..k]);
    k
}

/// One MSC data group of the MOT types.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_mot_parse_group(d: *const u8, n: usize, out: *mut MotGroup) {
    if !out.is_null() {
        *out = mot::parse_group(rd(d, n));
    }
}

/// The objects a MOT directory announces; the count (also past the capacity), `usize::MAX` when the directory is shorter than 13 bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_mot_parse_directory(d: *const u8, n: usize, out: *mut DirEntry, cap: usize) -> usize {
    mot::parse_directory(rd(d, n), wr(out, cap)).unwrap_or(usize::MAX)
}

/// The PAD of a DAB+ access unit (xpad needs 508 bytes); false when it carries none.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_pad_from_au(au: *const u8, n: usize, xpad: *mut u8, cap: usize, out: *mut PadBytes) -> bool {
    match pad::pad_from_au(rd(au, n), wr(xpad, cap)) {
        Some(p) if !out.is_null() => {
            *out = p;
            true
        }
        _ => false,
    }
}

/// The PAD of an MP2 frame (xpad needs 196 bytes); `last_cont` is updated like the reference does.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_pad_from_mp2(
    f: *const u8,
    n: usize,
    last_cont: *mut i32,
    xpad: *mut u8,
    cap: usize,
    out: *mut PadBytes,
) -> bool {
    if last_cont.is_null() {
        return false;
    }
    match pad::pad_from_mp2(rd(f, n), &mut *last_cont, wr(xpad, cap)) {
        Some(p) if !out.is_null() => {
            *out = p;
            true
        }
        _ => false,
    }
}

/// The subfields of one X-PAD (up to 5); `last_type` is updated like the reference does.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_pad_split(
    xp: *const u8,
    n: usize,
    fpad: *const u8,
    last_type: *mut i32,
    out: *mut PadSub,
    cap: usize,
) -> usize {
    let f = rd(fpad, 2);
    if f.len() < 2 || last_type.is_null() {
        return 0;
    }
    pad::pad_split(rd(xp, n), [f[0], f[1]], &mut *last_type, wr(out, cap))
}

/// The data group length indicator: 0 too short, 1 bad CRC, 2 good (`len` written).
#[no_mangle]
pub unsafe extern "C" fn onair_dab_pad_dg_length(d: *const u8, n: usize, len: *mut i32) -> u8 {
    let mut l = 0;
    let r = pad::dg_length_indicator(rd(d, n), &mut l);
    if r == 2 && !len.is_null() {
        *len = l;
    }
    r
}

/// What to do with the dynamic label subfield bytes collected so far.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_pad_dls_step(d: *const u8, n: usize, out: *mut DlsStep) {
    if !out.is_null() {
        *out = pad::dls_step(rd(d, n));
    }
}

/// The prefix of a dynamic label segment.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_pad_dl_segment(d: *const u8, n: usize, out: *mut DlSegment) {
    if !out.is_null() {
        *out = pad::dl_segment(rd(d, n));
    }
}

/// The DL Plus tags command (`out` has 4 entries): the number of tags, `usize::MAX` when too short; `running` is written.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_pad_dlplus_tags(d: *const u8, n: usize, out: *mut DlpTag, running: *mut u8) -> usize {
    let mut t = [DlpTag::default(); 4];
    match pad::dlplus_tags(rd(d, n), &mut t) {
        Some((r, c)) if !out.is_null() && !running.is_null() => {
            for (i, x) in t.iter().take(c).enumerate() {
                *out.add(i) = *x;
            }
            *running = r;
            c
        }
        _ => usize::MAX,
    }
}

/// The Journaline data group check.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_jl_data_group(d: *const u8, n: usize, out: *mut JlGroup) {
    if !out.is_null() {
        *out = jl::data_group(rd(d, n));
    }
}

/// A TOC management block; false when malformed.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_jl_parse_toc(d: *const u8, n: usize, info: *mut TocInfo, out: *mut TocEntry, cap: usize) -> bool {
    match jl::parse_toc(rd(d, n), wr(out, cap)) {
        Some(t) if !info.is_null() => {
            *info = t;
            true
        }
        _ => false,
    }
}

/// User application data of FIG 0/13.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_jl_user_app_data(
    d: *const u8,
    n: usize,
    version: *mut i32,
    ext_header: *mut i32,
    entry: *mut i32,
) -> bool {
    match jl::user_app_data(rd(d, n)) {
        Some((v, x, e)) if !version.is_null() && !ext_header.is_null() && !entry.is_null() => {
            *version = v;
            *ext_header = x;
            *entry = e;
            true
        }
        _ => false,
    }
}

/// The header fields of one DAB packet mode packet.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_packet(p: *const u8, n: usize, out: *mut DabPacket) {
    if !out.is_null() {
        *out = jl::dab_packet(rd(p, n));
    }
}

/// The header fields of one DRM packet mode packet.
#[no_mangle]
pub unsafe extern "C" fn onair_dab_drm_packet(p: *const u8, n: usize, out: *mut DrmPacket) {
    if !out.is_null() {
        *out = jl::drm_packet(rd(p, n));
    }
}
