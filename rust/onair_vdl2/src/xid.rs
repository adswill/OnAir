// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! XID frames (exchange identification, ICAO Doc 9776 section 6.3 / ISO 4335): the VDL parameter groups of ground station information frames (GSIF) and of link
//! establishment, handoff and parameter negotiation: parameter set, connection management (h and r bits), sequencing, airports, aircraft / ground station
//! locations (12 bit signed tenths of a degree), frequency and modulation lists.
use crate::ffi::cfn::format_f;
use crate::proto::{cstr_n, hexs, line, Parsed};
use onair_common::text::{Buf, Str};

/// 12 bit two's complement as a double.
fn signed12(v: i32) -> f64 {
    if v & 0x800 != 0 {
        (v - 0x1000) as f64
    } else {
        v as f64
    }
}

/// Reads an XID information field (public parameter group 0x80 and the VDL group 0xF0) and names the frame by its C/R, P/F, h and r bits (`m.response`,
/// `m.pf` are already set by the caller).
pub fn read_xid(p: &[u8], m: &mut Parsed) {
    let n = p.len();
    m.proto.set(b"XID");
    let (mut h, mut r) = (1i32, 1i32);
    let mut lines: Str<8192> = Str::default();
    let mut any = false;
    if n >= 3 && p[0] == 0x82 {
        let mut o = 1usize;
        while o + 3 <= n {
            let gid = p[o];
            let glen = (p[o + 1] as usize) << 8 | p[o + 2] as usize;
            o += 3;
            if o + glen > n {
                break;
            }
            let g = &p[o..o + glen];
            let mut k = 0usize;
            while k + 2 <= glen {
                let id = g[k];
                let len = g[k + 1] as usize;
                let v = &g[(k + 2).min(glen)..];
                if k + 2 + len > glen {
                    break;
                }
                let v = &v[..len];
                any = true;
                if gid == 0xF0 {
                    match id {
                        0x00 => line(&mut lines, |w| {
                            w.str("  parameter set ");
                            cstr_n(w, v);
                        }),
                        0x01 => {
                            if len >= 1 {
                                r = (v[0] & 1) as i32;
                                h = ((v[0] >> 1) & 1) as i32;
                                line(&mut lines, |w| {
                                    w.str("  connection management: h ");
                                    w.idec(h as i64);
                                    w.str(", r ");
                                    w.idec(r as i64);
                                });
                            }
                        }
                        0x03 => {
                            if len >= 1 {
                                line(&mut lines, |w| {
                                    w.str("  XID sequencing: seq ");
                                    w.udec((v[0] & 7) as u64);
                                    w.str(", retry ");
                                    w.udec((v[0] >> 4) as u64);
                                });
                            }
                        }
                        0x83 => line(&mut lines, |w| {
                            w.str("  destination airport ");
                            cstr_n(w, v);
                        }),
                        0xC1 => line(&mut lines, |w| {
                            w.str("  airport coverage ");
                            cstr_n(w, v);
                        }),
                        0xC3 => line(&mut lines, |w| {
                            w.str("  nearest airport ");
                            cstr_n(w, v);
                        }),
                        0x84 | 0xC8 => {
                            if len >= 3 {
                                let la = signed12(((v[0] as i32) << 4) | (v[1] as i32 >> 4)) / 10.0;
                                let lo = signed12((((v[1] & 0xF) as i32) << 8) | v[2] as i32) / 10.0;
                                m.has_pos = 1;
                                m.lat = la;
                                m.lon = lo;
                                let alt = id == 0x84 && len >= 4;
                                if alt {
                                    m.has_alt = 1;
                                    m.alt_ft = v[3] as i32 * 1000;
                                }
                                m.pos_kind.set(if id == 0x84 { b"XID aircraft location" } else { b"XID ground station location" });
                                line(&mut lines, |w| {
                                    w.str("  ");
                                    w.str(if id == 0x84 { "aircraft location" } else { "ground station location" });
                                    w.byte(b' ');
                                    w.fixed(format_f, 1, (if la < 0.0 { -la } else { la }));
                                    w.byte(if la < 0.0 { b'S' } else { b'N' });
                                    w.byte(b' ');
                                    w.fixed(format_f, 1, (if lo < 0.0 { -lo } else { lo }));
                                    w.byte(if lo < 0.0 { b'W' } else { b'E' });
                                    if alt {
                                        w.byte(b' ');
                                        w.idec(v[3] as i64 * 1000);
                                        w.str(" ft");
                                    }
                                });
                            }
                        }
                        0xC0 => line(&mut lines, |w| {
                            w.str("  frequency support list, ");
                            w.udec(len as u64);
                            w.str(" octets");
                        }),
                        0x82 => line(&mut lines, |w| {
                            w.str("  alternate ground stations, ");
                            w.udec(len as u64);
                            w.str(" octets");
                        }),
                        0x81 => line(&mut lines, |w| {
                            w.str("  modulation support ");
                            if len != 0 {
                                hexs(w, v, 24);
                            } else {
                                w.byte(b'-');
                            }
                        }),
                        _ => line(&mut lines, |w| {
                            w.str("  VDL parameter ");
                            w.hex(id as u64, 2, true);
                            w.str(": ");
                            hexs(w, v, 24);
                        }),
                    }
                } else if gid == 0x80 {
                    line(&mut lines, |w| {
                        w.str("  public parameter ");
                        w.udec(id as u64);
                        w.str(": ");
                        hexs(w, v, 24);
                    });
                }
                k += 2 + len;
            }
            o += glen;
        }
    }
    let kind = ((m.response as usize) << 3) | ((m.pf as usize) << 2) | ((h as usize) << 1) | r as usize;
    const NAMES: [&str; 16] = [
        "XID",
        "XID_CMD_LCR",
        "XID_CMD_HO",
        "GSIF",
        "XID_CMD_LE",
        "XID",
        "XID_CMD_HO",
        "XID_CMD_LPM",
        "XID",
        "XID",
        "XID",
        "XID",
        "XID_RSP_LE",
        "XID_RSP_LCR",
        "XID_RSP_HO",
        "XID_RSP_LPM",
    ];
    let name = NAMES[kind & 15];
    m.frame_name.set(name.as_bytes());
    let desc = match name {
        "GSIF" => "ground station information",
        "XID_CMD_LE" => "link establishment",
        "XID_RSP_LE" => "link establishment response",
        "XID_CMD_HO" => "handoff",
        "XID_RSP_HO" => "handoff response",
        "XID_CMD_LCR" => "link connection refused",
        _ => "link parameters",
    };
    m.summary.write(|w| {
        w.str(name);
        w.str(" (");
        w.str(desc);
        w.byte(b')');
    });
    line(&mut m.detail, |w| {
        w.str("XID: ");
        w.str(desc);
        if !any {
            w.str(", no parameters read");
        }
    });
    let l = lines;
    m.detail.append(|w: &mut Buf| w.bytes(l.as_bytes()));
}
