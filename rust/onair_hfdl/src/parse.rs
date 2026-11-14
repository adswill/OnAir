// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The PDU of one HFDL burst, read as text and fields (ARINC 635 / ICAO Doc 9741): SPDU squitters, MPDU headers (downlink and uplink), LPDUs (unnumbered data,
//! logon / logoff), HFNPDUs (performance data, frequency data, system table) with aircraft positions. The ACARS block inside an HFNPDU is only located
//! (`Msg::acars_at` / `acars_len`); the caller decodes it with the ACARS code and appends to the detail text.
use crate::ffi::cfn;
use crate::phy::{fcs, fcs_ok};
use crate::stations::{mask_khz, name_of, station};
use onair_common::text::{Buf, Str};

/// One decoded SPDU or LPDU, `repr(C)` (see rs/hfdl.h). Fields keep the `HfdlMessage` defaults (-1 / empty / 0) when the PDU does not set them.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Msg {
    pub squitter: u8,
    pub uplink: u8,
    pub utc_sync: u8,
    /// The HFNPDU holds an ACARS block.
    pub acars: u8,
    pub has_pos: u8,
    pub gs_id: i32,
    pub ac_id: i32,
    pub frame_index: i32,
    pub utc_sec: i32,
    /// Offset in the PDU of the octets after the HFNPDU's `FF FF` (the ACARS block, maybe starting with SOH); -1 none.
    pub acars_at: i32,
    /// Octets from there to the end of the LPDU body.
    pub acars_len: i32,
    /// Entries used in `active_khz`.
    pub active_n: i32,
    pub icao: u32,
    pub lat: f64,
    pub lon: f64,
    pub active_khz: [i32; 20],
    pub kind: Str<32>,
    pub hfnpdu: Str<24>,
    pub flight: Str<8>,
    pub pos_kind: Str<24>,
    pub summary: Str<256>,
    /// One line per layer.
    pub detail: Str<3072>,
}

impl Default for Msg {
    fn default() -> Self {
        Msg {
            squitter: 0,
            uplink: 0,
            utc_sync: 0,
            acars: 0,
            has_pos: 0,
            gs_id: 0,
            ac_id: -1,
            frame_index: -1,
            utc_sec: -1,
            acars_at: -1,
            acars_len: 0,
            active_n: 0,
            icao: 0,
            lat: 0.0,
            lon: 0.0,
            active_khz: [0; 20],
            kind: Str::default(),
            hfnpdu: Str::default(),
            flight: Str::default(),
            pos_kind: Str::default(),
            summary: Str::default(),
            detail: Str::default(),
        }
    }
}

const HDR_SPDU: usize = 66;

fn line(m: &mut Msg, f: impl FnOnce(&mut Buf)) {
    m.detail.append(|w| {
        f(w);
        w.byte(b'\n');
    });
}
fn d2(w: &mut Buf, v: i32) {
    if (0..10).contains(&v) {
        w.byte(b'0');
    }
    w.idec(v as i64);
}
fn fx(w: &mut Buf, prec: u8, v: f64) {
    w.fixed(cfn::format_f, prec, v);
}
fn id(w: &mut Buf, v: i32) {
    w.idec(v as i64);
}

/// `khz` list: frequencies, or `bitN` for entries beyond the table; "none" when empty.
fn khz_list(w: &mut Buf, k: &[i32]) {
    if k.is_empty() {
        w.str("none");
        return;
    }
    for (i, v) in k.iter().enumerate() {
        if i > 0 {
            w.byte(b' ');
        }
        if *v > 0 {
            id(w, *v);
        } else {
            w.str("bit");
            id(w, -*v - 1);
        }
    }
}
fn khz_mask(w: &mut Buf, gs: i32, mask: u32) {
    let mut t = [0i32; 20];
    let n = mask_khz(gs, mask, &mut t);
    khz_list(w, &t[..n]);
}
/// "<id> <name>" or "<id> (not in the table)".
fn gs_name(w: &mut Buf, gs: i32) {
    id(w, gs);
    w.byte(b' ');
    match station(gs) {
        Some(s) => w.bytes(name_of(s)),
        None => w.str("(not in the table)"),
    }
}

fn coord_deg(raw: u32) -> f64 {
    let mut v = (raw & 0xF_FFFF) as i32;
    if v & 0x8_0000 != 0 {
        v -= 0x10_0000;
    }
    v as f64 * 180.0 / 524288.0
}

/// A flight id, latitude, longitude (20-bit two's complement, 180 / 2^19 degrees per step) and UTC time (2 s steps) at `h[2..15]` of a performance / frequency
/// data HFNPDU (`h` has at least 15 octets).
fn read_pos_time(h: &[u8], m: &mut Msg, what: &str) {
    let mut fl = [0u8; 6];
    let mut n = 0;
    for &c in &h[2..8] {
        if (0x20..0x7F).contains(&c) {
            fl[n] = c;
            n += 1;
        }
    }
    let mut a = 0;
    while n > a && fl[n - 1] == b' ' {
        n -= 1;
    }
    while a < n && fl[a] == b' ' {
        a += 1;
    }
    let fl = &fl[a..n];
    let la = h[8] as u32 | (h[9] as u32) << 8 | (h[10] as u32 & 0xF) << 16;
    let lo = (h[10] as u32 & 0xF0) >> 4 | (h[11] as u32) << 4 | (h[12] as u32) << 12;
    let t = (h[13] as i32 | (h[14] as i32) << 8) * 2;
    m.flight.set(fl);
    m.lat = coord_deg(la);
    m.lon = coord_deg(lo);
    m.has_pos = (m.lat.abs() <= 90.0 && m.lon.abs() <= 180.0 && !(la == 0 && lo == 0)) as u8;
    m.pos_kind.set(what.as_bytes());
    m.utc_sec = t;
    let (lat, lon) = (m.lat, m.lon);
    line(m, |w| {
        w.str(what);
        w.str(": flight ");
        if fl.is_empty() {
            w.byte(b'-');
        } else {
            w.bytes(fl);
        }
        w.str(", position ");
        fx(w, 4, lat);
        w.byte(b' ');
        fx(w, 4, lon);
        w.str(", time ");
        d2(w, t / 3600 % 24);
        w.byte(b':');
        d2(w, t % 3600 / 60);
        w.byte(b':');
        d2(w, t % 60);
        w.str(" UTC");
    });
}

fn u(h: &[u8], i: usize) -> i32 {
    h[i] as i32
}

/// The HFNPDU at the start of `h` (`base`: its offset in the PDU).
fn read_hfnpdu(h: &[u8], base: usize, m: &mut Msg) {
    let n = h.len();
    if n < 2 || h[0] != 0xFF {
        m.summary.write(|w| {
            w.bytes(m.kind.as_bytes());
            w.str(" (");
            w.udec(n as u64);
            w.str(" octets, not an HFNPDU)");
        });
        line(m, |w| {
            w.str("  ");
            w.udec(n as u64);
            w.str(" octets of user data");
        });
        return;
    }
    match h[1] {
        0xFF => {
            m.hfnpdu.set(b"ACARS");
            m.acars = 1;
            m.acars_at = (base + 2) as i32;
            m.acars_len = (n - 2) as i32;
        }
        0xD1 => {
            m.hfnpdu.set(b"Performance data");
            if n < 47 {
                m.summary.set(b"Performance data (short)");
                return;
            }
            read_pos_time(h, m, "Performance data");
            line(m, |w| {
                w.str("  version ");
                id(w, u(h, 15));
                w.str(", flight leg ");
                id(w, u(h, 16));
                w.str(", ground station ");
                gs_name(w, u(h, 17) & 0x7F);
                w.str(", frequency id ");
                id(w, u(h, 18));
            });
            line(m, |w| {
                w.str("  MPDUs received 1800/1200/600/300:");
                for i in 27..31 {
                    w.byte(b' ');
                    id(w, u(h, i));
                }
                w.str(", with errors");
                for i in 31..35 {
                    w.byte(b' ');
                    id(w, u(h, i));
                }
                w.str("; SPDUs received ");
                id(w, u(h, 35) | u(h, 36) << 8);
                w.str(", missed ");
                id(w, u(h, 37));
            });
            let (fl, lat, lon) = (m.flight, m.lat, m.lon);
            m.summary.write(|w| {
                w.str("Performance data ");
                w.bytes(fl.as_bytes());
                w.str("  ");
                fx(w, 3, lat);
                w.byte(b' ');
                fx(w, 3, lon);
            });
        }
        0xD5 => {
            m.hfnpdu.set(b"Frequency data");
            if n < 15 {
                m.summary.set(b"Frequency data (short)");
                return;
            }
            read_pos_time(h, m, "Frequency data");
            let mut cnt = 0;
            let mut p = 15;
            while p + 6 <= n && cnt < 6 {
                let gs = u(h, p) & 0x7F;
                let heard = h[p + 1] as u32 | (h[p + 2] as u32) << 8 | (h[p + 3] as u32 & 0xF) << 16;
                let tuned = (h[p + 3] as u32 & 0xF0) >> 4 | (h[p + 4] as u32) << 4 | (h[p + 5] as u32) << 12;
                line(m, |w| {
                    w.str("  station ");
                    gs_name(w, gs);
                    w.str(": heard on ");
                    khz_mask(w, gs, heard);
                    w.str(" kHz, listening on ");
                    khz_mask(w, gs, tuned);
                    w.str(" kHz");
                });
                p += 6;
                cnt += 1;
            }
            let (fl, lat, lon) = (m.flight, m.lat, m.lon);
            m.summary.write(|w| {
                w.str("Frequency data ");
                w.bytes(fl.as_bytes());
                w.str("  ");
                fx(w, 3, lat);
                w.byte(b' ');
                fx(w, 3, lon);
                w.str(", ");
                id(w, cnt);
                w.str(" station");
                if cnt != 1 {
                    w.byte(b's');
                }
                w.str(" heard");
            });
        }
        0xD0 => {
            m.hfnpdu.set(b"System table");
            if n >= 5 {
                let mut s: Str<96> = Str::default();
                s.write(|w| {
                    w.str("System table part ");
                    id(w, u(h, 2) & 0xF);
                    w.str(" of ");
                    id(w, (u(h, 2) >> 4) + 1);
                    w.str(", version ");
                    id(w, (u(h, 3) >> 4) | u(h, 4) << 4);
                });
                m.summary.set(s.as_bytes());
                line(m, |w| {
                    w.str("  ");
                    w.bytes(s.as_bytes());
                });
            }
        }
        0xD2 => {
            m.hfnpdu.set(b"System table request");
            m.summary.set(b"System table request");
        }
        0xDE => {
            m.hfnpdu.set(b"Delayed echo");
            m.summary.set(b"Delayed echo");
        }
        t => {
            m.hfnpdu.set(b"HFNPDU");
            m.summary.write(|w| {
                w.str("HFNPDU type 0x");
                w.hex(t as u64, 2, true);
                w.str(" (");
                w.udec(n as u64);
                w.str(" octets)");
            });
        }
    }
}

/// The LPDU `p` (FCS already checked; `base`: its offset in the PDU).
fn read_lpdu(p: &[u8], base: usize, m: &mut Msg) {
    let len = p.len() - 2;
    let t = p[0];
    // The three ICAO-address octets in logon/logoff LPDUs are sent MSB-first.
    // Reverse the bits within each octet; preserve the octet order.
    let icao = |at: usize| {
        if len >= at + 3 {
            ((p[at].reverse_bits() as u32) << 16)
                | ((p[at + 1].reverse_bits() as u32) << 8)
                | p[at + 2].reverse_bits() as u32
        } else {
            0
        }
    };
    match t {
        0x0D | 0x1D => {
            m.kind.set(if t == 0x0D { b"Unnumbered data" as &[u8] } else { b"Unnumbered acked data" });
            let k = m.kind;
            line(m, |w| {
                w.str("LPDU: ");
                w.bytes(k.as_bytes());
                w.str(", ");
                w.udec((len - 1) as u64);
                w.str(" octets");
            });
            read_hfnpdu(&p[1..len], base + 1, m);
        }
        0x8F | 0xBF | 0x4F => {
            m.kind.set(match t {
                0x8F => b"Logon request" as &[u8],
                0xBF => b"Logon request (DLS)",
                _ => b"Logon resume",
            });
            m.icao = icao(1);
            let (k, ic) = (m.kind, m.icao);
            m.summary.write(|w| {
                w.bytes(k.as_bytes());
                w.str(" from ");
                w.hex(ic as u64, 6, true);
            });
            let s = m.summary;
            line(m, |w| {
                w.str("LPDU: ");
                w.bytes(s.as_bytes());
            });
        }
        0x9F | 0x5F => {
            m.kind.set(if t == 0x9F { b"Logon confirm" as &[u8] } else { b"Logon resume confirm" });
            m.icao = icao(1);
            if len >= 5 {
                m.ac_id = p[4] as i32;
            }
            let (k, ic, ac) = (m.kind, m.icao, m.ac_id);
            m.summary.write(|w| {
                w.bytes(k.as_bytes());
                w.byte(b' ');
                w.hex(ic as u64, 6, true);
                w.str(", aircraft id ");
                id(w, ac);
            });
            let s = m.summary;
            line(m, |w| {
                w.str("LPDU: ");
                w.bytes(s.as_bytes());
            });
        }
        0x2F | 0x3F => {
            m.kind.set(if t == 0x2F { b"Logon denied" as &[u8] } else { b"Logoff request" });
            m.icao = icao(1);
            let (k, ic) = (m.kind, m.icao);
            let reason = if len >= 5 { p[4] as i32 } else { -1 };
            m.summary.write(|w| {
                w.bytes(k.as_bytes());
                w.byte(b' ');
                w.hex(ic as u64, 6, true);
                w.str(", reason ");
                id(w, reason);
            });
            let s = m.summary;
            line(m, |w| {
                w.str("LPDU: ");
                w.bytes(s.as_bytes());
            });
        }
        _ => {
            m.kind.set(b"LPDU");
            m.summary.write(|w| {
                w.str("LPDU type 0x");
                w.hex(t as u64, 2, true);
                w.str(" (");
                w.udec(len as u64);
                w.str(" octets)");
            });
            let s = m.summary;
            line(m, |w| w.bytes(s.as_bytes()));
        }
    }
}

fn push(out: &mut [Msg], count: &mut usize, m: Msg) {
    if *count < out.len() {
        out[*count] = m;
        *count += 1;
    }
}

fn spdu(p: &[u8], out: &mut [Msg], count: &mut usize) -> bool {
    if p.len() < HDR_SPDU || !fcs_ok(&p[..66]) {
        return false;
    }
    let mut m = Msg { squitter: 1, uplink: 1, ..Msg::default() };
    m.kind.set(b"SPDU");
    m.gs_id = (p[1] & 0x7F) as i32;
    m.utc_sync = p[1] >> 7;
    m.frame_index = p[2] as i32 | (p[3] as i32 & 0xF) << 8;
    const NOTES: [&str; 4] = ["none", "channel down", "upcoming frequency change", "ground station down"];
    let mask0 = (p[54] as u32) >> 4 | (p[55] as u32) << 4 | (p[56] as u32) << 12;
    let mut t20 = [0i32; 20];
    let an = mask_khz(m.gs_id, mask0, &mut t20);
    m.active_khz = t20;
    m.active_n = an as i32;
    let t = m.frame_index * 32;
    let (gs, fi, sync) = (m.gs_id, m.frame_index, m.utc_sync != 0);
    line(&mut m, |w| {
        w.str("SPDU (squitter) from ground station ");
        gs_name(w, gs);
        w.str(if sync { ", UTC synchronised" } else { ", not UTC synchronised" });
    });
    line(&mut m, |w| {
        w.str("  version ");
        id(w, (p[0] as i32 >> 2) & 3);
        w.str(", change note: ");
        w.str(NOTES[(p[0] as usize >> 6) & 3]);
        w.str(", RLS ");
        w.str(if p[0] & 2 != 0 { "in use" } else { "not in use" });
        w.str(", ISO 8208 ");
        w.str(if p[0] & 0x20 != 0 { "supported" } else { "not supported" });
    });
    line(&mut m, |w| {
        w.str("  TDMA frame ");
        id(w, fi);
        w.str(" (");
        d2(w, t / 3600 % 24);
        w.byte(b':');
        d2(w, t % 3600 / 60);
        w.byte(b':');
        d2(w, t % 60);
        w.str(" UTC at 32 s per frame), offset ");
        id(w, (p[3] >> 4) as i32);
        w.str(", minimum priority ");
        id(w, (p[52] & 0xF) as i32);
        w.str(", system table version ");
        id(w, p[53] as i32 | (p[54] as i32 & 0xF) << 8);
    });
    line(&mut m, |w| {
        w.str("  frequencies in use: ");
        khz_list(w, &t20[..an]);
        w.str(" kHz");
    });
    let id1 = (p[57] & 0x7F) as i32;
    let id2 = (p[60] as i32 >> 4) | (p[61] as i32 & 7) << 4;
    let m1 = p[58] as u32 | (p[59] as u32) << 8 | (p[60] as u32 & 0xF) << 16;
    let m2 = (p[61] as u32) >> 4 | (p[62] as u32) << 4 | (p[63] as u32) << 12;
    if id1 != 0 {
        line(&mut m, |w| {
            w.str("  station ");
            gs_name(w, id1);
            w.str(if p[57] & 0x80 != 0 { " (UTC)" } else { "" });
            w.str(": ");
            khz_mask(w, id1, m1);
            w.str(" kHz");
        });
    }
    if id2 != 0 {
        line(&mut m, |w| {
            w.str("  station ");
            gs_name(w, id2);
            w.str(if p[61] & 8 != 0 { " (UTC)" } else { "" });
            w.str(": ");
            khz_mask(w, id2, m2);
            w.str(" kHz");
        });
    }
    m.summary.write(|w| {
        w.str("Squitter ");
        match station(gs) {
            Some(s) => w.bytes(name_of(s)),
            None => {
                w.str("GS ");
                id(w, gs);
            }
        }
        w.str(", frame ");
        id(w, fi);
        w.str(", ");
        w.udec(an as u64);
        w.str(" frequencies in use");
    });
    push(out, count, m);
    true
}

/// Parses the PDU of one burst (padding after it allowed) into up to `out.len()` messages; returns how many, or `None` when no check sequence comes out right.
pub fn parse_pdu(p: &[u8], out: &mut [Msg]) -> Option<usize> {
    let n = p.len();
    if n < 4 {
        return None;
    }
    let mut count = 0usize;
    if p[0] & 1 == 0 {
        return if spdu(p, out, &mut count) { Some(count) } else { None };
    }
    let down = p[0] & 2 != 0;
    // (aircraft id, LPDU length)
    let mut parts = [(0i32, 0usize); 128];
    let mut np = 0usize;
    let gs = (p[1] & 0x7F) as i32;
    let hdr;
    if down {
        let cnt = ((p[0] >> 2) & 0xF) as usize;
        hdr = 6 + cnt;
        if n < hdr + 2 {
            return None;
        }
        for i in 0..cnt {
            parts[np] = (p[2] as i32, p[6 + i] as usize + 1);
            np += 1;
        }
    } else {
        let nac = ((p[0] & 0x70) >> 4) as usize + 1;
        let mut at = 2usize;
        for _ in 0..nac {
            if n < at + 2 {
                return None;
            }
            let ac = p[at] as i32;
            let cnt = ((p[at + 1] >> 4) & 0xF) as usize;
            if n < at + 2 + cnt {
                return None;
            }
            for i in 0..cnt {
                parts[np] = (ac, p[at + 2 + i] as usize + 1);
                np += 1;
            }
            at += 2 + cnt;
        }
        hdr = at;
    }
    if n < hdr + 2 || !fcs_ok(&p[..hdr + 2]) {
        onair_common::diag_debug!(487122, "An HFDL frame of {} bytes is too short or failed its frame check, so it was dropped.", n);
        return None;
    }
    let mut at = hdr + 2;
    for &(ac, len) in &parts[..np] {
        if at + len > n {
            break;
        }
        let mut m = Msg { uplink: (!down) as u8, gs_id: gs, ac_id: ac, ..Msg::default() };
        line(&mut m, |w| {
            w.str("MPDU ");
            w.str(if down { "downlink" } else { "uplink" });
            w.str(", ground station ");
            gs_name(w, gs);
            w.str(", aircraft id ");
            id(w, ac);
        });
        if len >= 3 && fcs_ok(&p[at..at + len]) {
            read_lpdu(&p[at..at + len], at, &mut m);
        } else {
            m.kind.set(b"LPDU");
            m.summary.set(b"LPDU with a bad check sequence");
            line(&mut m, |w| w.str("LPDU: check sequence FAILED"));
        }
        push(out, &mut count, m);
        at += len;
    }
    if np == 0 {
        let mut m = Msg { uplink: (!down) as u8, gs_id: gs, ac_id: if down { p[2] as i32 } else { -1 }, ..Msg::default() };
        m.kind.set(b"MPDU");
        m.summary.set(b"MPDU without LPDUs");
        push(out, &mut count, m);
    }
    Some(count)
}

/// Appends the FCS to a copy of `p` in `out` (test helper).
pub fn put_fcs(p: &[u8], out: &mut [u8]) -> usize {
    let f = fcs(p);
    out[..p.len()].copy_from_slice(p);
    out[p.len()] = f as u8;
    out[p.len() + 1] = (f >> 8) as u8;
    p.len() + 2
}
