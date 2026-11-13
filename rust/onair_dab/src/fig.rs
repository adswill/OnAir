// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! DAB fast information channel parsing (ETSI EN 300 401 clause 5.2 and 8, ETSI TS 101 756): the FIB walk, the FIG headers, FIG 0 extensions 0, 1, 2, 3, 10,
//! 13, 17, 18 and 19 (ensemble id, sub-channel organisation, service organisation, packet mode components, date and time, user applications, programme type,
//! announcement support and switching) and the label FIGs 1 and 2. Parsers return plain records; the ensemble tables, label segment store and the
//! sub-channel size / bit rate tables stay in C++.
use crate::ffi_types::*;

/// Byte `i` of `b`, 0 past the end.
#[inline]
fn rd(b: &[u8], i: usize) -> i32 {
    b.get(i).copied().unwrap_or(0) as i32
}

/// 16 bit big-endian value at `i`.
#[inline]
fn rd16(b: &[u8], i: usize) -> u32 {
    ((rd(b, i) as u32) << 8) | rd(b, i + 1) as u32
}

/// 32 bit big-endian value at `i`.
#[inline]
fn rd32(b: &[u8], i: usize) -> u32 {
    (rd16(b, i) << 16) | rd16(b, i + 2)
}

/// Bits `pos..pos+n` of a packed MSB first byte string (0 past the end).
fn get_bits(b: &[u8], pos: usize, n: usize) -> u32 {
    let mut v = 0u32;
    for i in 0..n {
        let p = pos + i;
        v = (v << 1) | ((rd(b, p >> 3) >> (7 - (p & 7))) & 1) as u32;
    }
    v
}

/// Walk the FIGs of a 30 byte FIB data field (the CRC is not part of it): each FIG is type (3 bits), length (5 bits) and data; 0xFF ends the list and a FIG
/// that runs past byte 30 stops it. Returns the bytes used (at most 30); FIGs of length 0 are skipped.
pub fn fib_walk(fib: &[u8], out: &mut [FigRef]) -> (usize, usize) {
    let mut i = 0usize;
    let mut n = 0usize;
    while i < 30 && rd(fib, i) != 0xFF {
        let (typ, len) = (rd(fib, i) >> 5, (rd(fib, i) & 31) as usize);
        if i + 1 + len > 30 {
            break;
        }
        let off = i + 1;
        i += 1 + len;
        if len < 1 {
            continue;
        }
        if n < out.len() {
            out[n] = FigRef { typ, off: off as u32, len: len as u32 };
        }
        n += 1;
    }
    (n, i.min(30))
}

/// The header fields of a FIG as the receiver files it: C/N, other ensemble, P/D and the extension (FIG 0 / 1 / 2 / 5; others 0).
pub fn fig_head(typ: i32, d: &[u8]) -> FigHead {
    let b = rd(d, 0);
    match typ {
        0 => FigHead { cn: b >> 7, oe: (b >> 6) & 1, pd: (b >> 5) & 1, ext: b & 31 },
        1 => FigHead { cn: 0, oe: (b >> 3) & 1, pd: 0, ext: b & 7 },
        2 | 5 => FigHead { cn: 0, oe: 0, pd: 0, ext: b & 7 },
        _ => FigHead::default(),
    }
}

/// FIG 0/0 ensemble identifier (`d` = the whole FIG data field incl. the header byte, `n = d.len() - 1`).
pub fn fig0_eid(d: &[u8]) -> Option<i32> {
    (d.len() >= 3).then(|| rd16(d, 1) as i32)
}

/// FIG 0/1 sub-channel organisation: up to `out.len()` entries; the count of all entries is returned. The caller computes size / bit rate from the protection.
pub fn fig0_subchannels(d: &[u8], out: &mut [Fig01Sub]) -> usize {
    let b = d.get(1..).unwrap_or(&[]);
    let n = b.len();
    let (mut p, mut c) = (0usize, 0usize);
    while p + 3 <= n {
        let mut s = Fig01Sub { id: rd(b, p) >> 2, start: ((rd(b, p) & 3) << 8) | rd(b, p + 1), ..Default::default() };
        if rd(b, p + 2) & 0x80 != 0 {
            if p + 4 > n {
                break;
            }
            s.eep = 1;
            s.option = (rd(b, p + 2) >> 4) & 7;
            s.level = (rd(b, p + 2) >> 2) & 3;
            s.size = ((rd(b, p + 2) & 3) << 8) | rd(b, p + 3);
            p += 4;
        } else {
            s.uep_index = rd(b, p + 2) & 0x3F;
            s.uep_table = ((rd(b, p + 2) & 0x40) != 0) as u8;
            p += 3;
        }
        if c < out.len() {
            out[c] = s;
        }
        c += 1;
    }
    c
}

/// FIG 0/3 packet mode service components.
pub fn fig0_packet(d: &[u8], out: &mut [Fig03Packet]) -> usize {
    let b = d.get(1..).unwrap_or(&[]);
    let n = b.len();
    let (mut p, mut c) = (0usize, 0usize);
    while p + 5 <= n {
        let e = Fig03Packet {
            scid: (rd(b, p) << 4) | (rd(b, p + 1) >> 4),
            dg: ((rd(b, p + 2) & 0x80) == 0) as u8,
            dscty: rd(b, p + 2) & 0x3F,
            sub_id: rd(b, p + 3) >> 2,
            address: ((rd(b, p + 3) & 3) << 8) | rd(b, p + 4),
        };
        let ca = rd(b, p + 1) & 1 != 0;
        if c < out.len() {
            out[c] = e;
        }
        c += 1;
        p += 5 + if ca { 2 } else { 0 };
    }
    c
}

/// FIG 0/2 service organisation: services and their components (flat arrays; a service owns `ncomp` consecutive components from `first`).
pub fn fig0_services(d: &[u8], svcs: &mut [Fig02Service], comps: &mut [Fig02Comp]) -> (usize, usize) {
    let pd = (rd(d, 0) >> 5) & 1;
    let b = d.get(1..).unwrap_or(&[]);
    let n = b.len();
    let (mut p, mut ns, mut nc) = (0usize, 0usize, 0usize);
    while p + 3 <= n {
        let sid;
        if pd == 0 {
            sid = rd16(b, p);
            p += 2;
        } else {
            if p + 5 > n {
                break;
            }
            sid = rd32(b, p);
            p += 4;
        }
        let ncomp = (rd(b, p) & 15) as usize;
        p += 1;
        let first = nc;
        let mut k = 0;
        while k < ncomp && p + 2 <= n {
            let mut c = Fig02Comp { tmid: rd(b, p) >> 6, ..Default::default() };
            match c.tmid {
                0 => {
                    c.ascty = rd(b, p) & 63;
                    c.sub_id = rd(b, p + 1) >> 2;
                    c.primary = ((rd(b, p + 1) >> 1) & 1) as u8;
                }
                3 => {
                    c.scid = ((rd(b, p) & 0x3F) << 6) | (rd(b, p + 1) >> 2);
                    c.primary = ((rd(b, p + 1) >> 1) & 1) as u8;
                }
                1 => {
                    c.dscty = rd(b, p) & 63;
                    c.sub_id = rd(b, p + 1) >> 2;
                    c.primary = ((rd(b, p + 1) >> 1) & 1) as u8;
                }
                _ => {}
            }
            if nc < comps.len() {
                comps[nc] = c;
            }
            nc += 1;
            k += 1;
            p += 2;
        }
        if ns < svcs.len() {
            svcs[ns] = Fig02Service { sid, first: first as u32, ncomp: (nc - first) as u32 };
        }
        ns += 1;
    }
    (ns, nc)
}

/// FIG 0/13 user application entries: per application the service, SCIdS, its index in the service's list, the type and the byte range of its data.
pub fn fig0_userapps(d: &[u8], out: &mut [Fig013App]) -> usize {
    let pd = (rd(d, 0) >> 5) & 1;
    let b = d.get(1..).unwrap_or(&[]);
    let n = b.len();
    let (mut p, mut c) = (0usize, 0usize);
    let need = if pd != 0 { 5 } else { 3 };
    while p + need <= n {
        let sid;
        if pd == 0 {
            sid = rd16(b, p);
            p += 2;
        } else {
            sid = rd32(b, p);
            p += 4;
        }
        let scids = rd(b, p) >> 4;
        let nua = (rd(b, p) & 15) as usize;
        p += 1;
        let mut u = 0;
        while u < nua && p + 2 <= n {
            let typ = (rd(b, p) << 3) | (rd(b, p + 1) >> 5);
            let len = (rd(b, p + 1) & 31) as usize;
            p += 2;
            if p + len > n {
                break;
            }
            if c < out.len() {
                out[c] = Fig013App { sid, scids, index: u as i32, typ, off: (p + 1) as u32, len: len as u32 };
            }
            c += 1;
            p += len;
            u += 1;
        }
    }
    c
}

/// FIG 0/17 programme type entries.
pub fn fig0_pty(d: &[u8], out: &mut [Fig017Pty]) -> usize {
    let b = d.get(1..).unwrap_or(&[]);
    let n = b.len();
    let (mut p, mut c) = (0usize, 0usize);
    while p + 3 <= n {
        let sid = rd16(b, p);
        let (sd, primary, l, cc) = (rd(b, p + 2) & 0x80 != 0, rd(b, p + 2) & 0x40 == 0, rd(b, p + 2) & 0x20 != 0, rd(b, p + 2) & 0x10 != 0);
        p += 3;
        let (mut lang, mut pty) = (-1, -1);
        if l && p < n {
            lang = rd(b, p);
            p += 1;
        }
        if p < n {
            pty = rd(b, p) & 0x1F;
            p += 1;
        }
        if cc {
            p += 1;
        }
        if c < out.len() {
            out[c] = Fig017Pty { sid, sd: sd as u8, primary: primary as u8, lang, pty };
        }
        c += 1;
    }
    c
}

/// FIG 0/18 announcement support: entries (`ncl` clusters stored from `first` in the cluster array).
pub fn fig0_ann_support(d: &[u8], out: &mut [Fig018Support], clusters: &mut [u8]) -> (usize, usize) {
    let b = d.get(1..).unwrap_or(&[]);
    let n = b.len();
    let (mut p, mut c, mut k) = (0usize, 0usize, 0usize);
    while p + 5 <= n {
        let sid = rd16(b, p);
        let asu = rd16(b, p + 2);
        let nc = (rd(b, p + 4) & 0x1F) as usize;
        p += 5;
        let first = k;
        let mut q = 0;
        while q < nc && p < n {
            if k < clusters.len() {
                clusters[k] = rd(b, p) as u8;
            }
            k += 1;
            q += 1;
            p += 1;
        }
        if c < out.len() {
            out[c] = Fig018Support { sid, asu, first: first as u32, ncl: (k - first) as u32 };
        }
        c += 1;
    }
    (c, k)
}

/// FIG 0/19 announcement switching entries.
pub fn fig0_ann_switch(d: &[u8], out: &mut [Fig019Switch]) -> usize {
    let b = d.get(1..).unwrap_or(&[]);
    let n = b.len();
    let (mut p, mut c) = (0usize, 0usize);
    while p + 4 <= n {
        let sw = Fig019Switch {
            cluster: rd(b, p),
            asw: rd16(b, p + 1),
            new_flag: ((rd(b, p + 3) & 0x80) != 0) as u8,
            region: ((rd(b, p + 3) & 0x40) != 0) as u8,
            sub_id: rd(b, p + 3) & 0x3F,
        };
        p += 4 + sw.region as usize;
        if c < out.len() {
            out[c] = sw;
        }
        c += 1;
    }
    c
}

/// FIG 0/10 date and time: the MJD, the time and the seconds since 1970 (0 when the MJD is not after 1 Jan 1970).
pub fn fig0_time(d: &[u8]) -> Option<Fig010Time> {
    let b = d.get(1..).unwrap_or(&[]);
    let n = b.len();
    if n < 4 {
        return None;
    }
    let mjd = get_bits(b, 1, 17);
    let flag = get_bits(b, 20, 1);
    let (hh, mm) = (get_bits(b, 21, 5) as i32, get_bits(b, 26, 6) as i32);
    let ss = if flag != 0 && n >= 6 { get_bits(b, 32, 6) as i32 } else { 0 };
    let utc = if mjd > 40587 { (mjd as i64 - 40587) * 86400 + (hh * 3600 + mm * 60 + ss) as i64 } else { 0 };
    Some(Fig010Time { mjd, hour: hh, minute: mm, second: ss, utc })
}

/// FIG 1 label: which entity it names and where the 16 label bytes are. `None` for the labels the receiver does not follow or that are too short.
pub fn fig1(d: &[u8]) -> Option<Fig1Label> {
    let len = d.len();
    let (ext, cs) = (rd(d, 0) & 7, rd(d, 0) >> 4);
    let n = len.saturating_sub(1);
    let b = d.get(1..).unwrap_or(&[]);
    let mut l = Fig1Label { ext, charset: cs, ..Default::default() };
    match ext {
        0 if n >= 18 => l.off = 3,
        1 if n >= 18 => {
            l.sid = rd16(b, 0);
            l.off = 3;
        }
        5 if n >= 20 => {
            l.sid = rd32(b, 0);
            l.off = 5;
        }
        4 if n >= 20 => {
            let pd = rd(b, 0) & 0x80 != 0;
            l.scids = rd(b, 0) & 0x0F;
            let il = if pd { 4 } else { 2 };
            if n < 1 + il + 18 {
                return None;
            }
            l.sid = if pd { rd32(b, 1) } else { rd16(b, 1) };
            l.off = (2 + il) as u32;
        }
        _ => return None,
    }
    Some(l)
}

/// FIG 2 (extended label segment): the identity of the label and the byte range of this segment's text, with the segment 0 fields.
pub fn fig2(d: &[u8]) -> Option<Fig2Seg> {
    let len = d.len();
    if len < 1 {
        return None;
    }
    let (toggle, seg, rfu, ext) = (rd(d, 0) >> 7, (rd(d, 0) >> 4) & 7, (rd(d, 0) >> 3) & 1, rd(d, 0) & 7);
    let idl = match ext {
        0 | 1 => 2,
        4 => {
            if len > 1 && rd(d, 1) & 0x80 != 0 {
                5
            } else {
                3
            }
        }
        5 => 4,
        _ => return None,
    };
    if len <= 1 + idl {
        return None;
    }
    let mut r = Fig2Seg { ext, toggle, seg, rfu, ..Default::default() };
    match ext {
        1 => r.sid = rd16(d, 1),
        5 => r.sid = rd32(d, 1),
        4 => {
            r.scids = rd(d, 1) & 0x0F;
            r.sid = if idl == 5 { rd32(d, 2) } else { rd16(d, 2) };
        }
        _ => {}
    }
    let mut p = 1 + idl;
    let mut n = len - 1 - idl;
    if seg == 0 {
        r.ucs2 = (rd(d, p) >> 7) as u8;
        r.total = ((rd(d, p) >> 4) & 7) + 1;
        let skip = if rfu != 0 { 1 } else { 3 };
        if n <= skip {
            r.valid = 0;
            r.text_off = p as u32;
            r.text_len = 0;
            return Some(r);
        }
        p += skip;
        n -= skip;
    }
    r.valid = 1;
    r.text_off = p as u32;
    r.text_len = n as u32;
    Some(r)
}
