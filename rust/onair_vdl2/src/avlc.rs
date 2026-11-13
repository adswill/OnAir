// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! AVLC (Aviation VHF Link Control, ICAO Annex 10 Vol III Part I section 6.3.4, Doc 9776): the two 4-octet addresses with the A/G and C/R bits, the control octet
//! (I, S and U frames) and what the information field carries. Frames with a good FCS only (the caller strips the two FCS octets).
use crate::bits::rev;
use crate::proto::{self, Parsed};
use crate::xid;

/// An AVLC address: 24 bit address, 3 bit type and the A/G (destination) or C/R (source) status bit.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Addr {
    pub addr: u32,
    pub kind: i32,
    pub status: u8,
}

impl Default for Addr {
    fn default() -> Self {
        Addr { addr: 0, kind: 1, status: 0 }
    }
}

/// Writes an address into 4 octets: the 28 bit value is bit-reversed and sent 7 bits per octet, the last octet carries the HDLC address-end bit.
pub fn put_addr(a: &Addr, last: bool) -> [u8; 4] {
    let v: u32 = ((a.status as u32) << 27) | (((a.kind & 7) as u32) << 24) | (a.addr & 0xFF_FFFF);
    let t = rev(v, 28);
    let mut o = [0u8; 4];
    for (i, b) in o.iter_mut().enumerate() {
        *b = (((t >> (7 * i)) & 0x7F) << 1) as u8;
    }
    if last {
        o[3] |= 1;
    }
    o
}

/// Reads an address from 4 octets.
pub fn get_addr(o: &[u8; 4]) -> Addr {
    let mut t = 0u32;
    for (i, b) in o.iter().enumerate() {
        t |= (((b >> 1) & 0x7F) as u32) << (7 * i);
    }
    let v = rev(t, 28);
    Addr { addr: v & 0xFF_FFFF, kind: ((v >> 24) & 7) as i32, status: ((v >> 27) & 1) as u8 }
}

/// Address type name with a trailing NUL ("aircraft", "ground station (ADM)", ...).
pub fn addr_type_name_z(t: i32) -> &'static str {
    match t {
        1 => "aircraft\0",
        4 => "ground station (ADM)\0",
        5 => "ground station (DEL)\0",
        7 => "all stations\0",
        _ => "reserved\0",
    }
}

/// Address type name.
pub fn addr_type_name(t: i32) -> &'static str {
    let s = addr_type_name_z(t);
    &s[..s.len() - 1]
}

/// True when the information field starts an ACARS block: the 0xFF 0xFF prefix of ARINC 618 followed by SOH.
fn acars_info(info: &[u8]) -> bool {
    info.len() > 3 && info[0] == 0xFF && info[1] == 0xFF && info[2] == 0x01
}

/// Decodes a frame of `n >= 9` octets (without FCS): addresses, control and the information field up to the point where ACARS decoding takes over (reported
/// by `acars_at`, the offset of the SOH). `None` when the frame is too short to be AVLC.
pub fn parse_frame(f: &[u8]) -> Option<Parsed> {
    let n = f.len();
    if n < 9 {
        return None;
    }
    let mut m = Parsed::default();
    let d = get_addr(&[f[0], f[1], f[2], f[3]]);
    let s = get_addr(&[f[4], f[5], f[6], f[7]]);
    m.dst = d.addr;
    m.dst_type = d.kind;
    m.src = s.addr;
    m.src_type = s.kind;
    m.on_ground = d.status;
    m.response = s.status;
    m.from_air = (s.kind == 1) as u8;
    m.from_ground = (s.kind == 4 || s.kind == 5) as u8;
    let c = f[8];
    let info = &f[9..];
    let inn = info.len();
    m.ok = 1;
    let (on_ground, response) = (m.on_ground, m.response);
    proto::line(&mut m.detail, |w| {
        w.str("AVLC: ");
        w.hex(s.addr as u64, 6, true);
        w.str(" (");
        w.str(addr_type_name(s.kind));
        w.str(") to ");
        w.hex(d.addr as u64, 6, true);
        w.str(" (");
        w.str(addr_type_name(d.kind));
        w.str("), ");
        w.str(if on_ground != 0 { "on ground" } else { "airborne" });
        w.str(", ");
        w.str(if response != 0 { "response" } else { "command" });
    });
    if c & 1 == 0 {
        m.frame_kind = b'I';
        m.frame_name.set(b"I");
        m.ns = ((c >> 1) & 7) as i32;
        m.nr = ((c >> 5) & 7) as i32;
        m.pf = (c >> 4) & 1;
        let (ns, nr, pf) = (m.ns, m.nr, m.pf);
        proto::line(&mut m.detail, |w| {
            w.str("  I frame, N(S) ");
            w.idec(ns as i64);
            w.str(", N(R) ");
            w.idec(nr as i64);
            if pf != 0 {
                w.str(", poll");
            }
            w.str(", ");
            w.udec(inn as u64);
            w.str(" octets of information");
        });
        if acars_info(info) {
            m.acars_at = 11;
        } else if inn != 0 {
            proto::read_x25(info, &mut m);
        } else {
            m.summary.set(b"I frame, no information");
        }
    } else if c & 3 == 1 {
        const SN: [&str; 4] = ["RR", "RNR", "REJ", "SREJ"];
        m.frame_kind = b'S';
        m.frame_name.set(SN[((c >> 2) & 3) as usize].as_bytes());
        m.nr = ((c >> 5) & 7) as i32;
        m.pf = (c >> 4) & 1;
        let (name, nr, pf) = (m.frame_name, m.nr, m.pf);
        m.summary.write(|w| {
            w.bytes(name.as_bytes());
            w.str(" N(R) ");
            w.idec(nr as i64);
        });
        proto::line(&mut m.detail, |w| {
            w.str("  S frame ");
            w.bytes(name.as_bytes());
            w.str(", N(R) ");
            w.idec(nr as i64);
            if pf != 0 {
                w.str(", poll/final");
            }
        });
    } else {
        m.frame_kind = b'U';
        m.pf = (c >> 4) & 1;
        let mf = (c >> 2) & 0x3B;
        let name: &[u8] = match mf {
            0x00 => b"UI",
            0x03 => b"DM",
            0x10 => b"DISC",
            0x18 => b"UA",
            0x21 => b"FRMR",
            0x2B => b"XID",
            0x38 => b"TEST",
            _ => b"U?",
        };
        m.frame_name.set(name);
        let pf = m.pf;
        proto::line(&mut m.detail, |w| {
            w.str("  U frame ");
            w.bytes(name);
            if pf != 0 {
                w.str(", poll/final");
            }
            w.str(", ");
            w.udec(inn as u64);
            w.str(" octets of information");
        });
        if mf == 0x2B {
            xid::read_xid(info, &mut m);
        } else if mf == 0x00 && acars_info(info) {
            m.acars_at = 11;
        } else {
            m.summary.write(|w| {
                w.bytes(name);
                if inn != 0 {
                    w.str(" (");
                    w.udec(inn as u64);
                    w.str(" octets)");
                }
            });
        }
    }
    Some(m)
}
