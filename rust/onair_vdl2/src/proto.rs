// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The protocols an AVLC information field carries, as text for the detail pane: ISO 8208 / X.25 packets (data, call set-up, clear, reset, restart, flow
//! control), ISO 8473 CLNP headers with the NSAP addresses and the ISO 8073 COTP TPDU around ATN application data (ASN.1 PER, not decoded). ICAO Doc 9880
//! (ATN/OSI), Doc 9776 (VDL Mode 2 and the SNDCF), ISO 8208, ISO 8473, ISO 8073.
use onair_common::text::{Buf, Str};

/// The decoded part of an AVLC frame, `repr(C)` (see rs/vdl2.h). Fields keep the `Vdl2Message` defaults (-1 / empty / 0) when the frame does not set them.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Parsed {
    /// The frame was long enough to be AVLC.
    pub ok: u8,
    pub from_air: u8,
    pub from_ground: u8,
    pub on_ground: u8,
    pub response: u8,
    pub pf: u8,
    pub has_pos: u8,
    pub has_alt: u8,
    /// 'I', 'S' or 'U'.
    pub frame_kind: u8,
    pub dst_type: i32,
    pub src_type: i32,
    /// I / S frame sequence numbers (-1: not in this frame).
    pub ns: i32,
    pub nr: i32,
    pub alt_ft: i32,
    /// Offset in the frame of the SOH of an ACARS block that the caller decodes (-1 none).
    pub acars_at: i32,
    pub dst: u32,
    pub src: u32,
    pub lat: f64,
    pub lon: f64,
    pub frame_name: Str<16>,
    pub proto: Str<16>,
    pub summary: Str<512>,
    pub pos_kind: Str<64>,
    /// The decoded layers, one line each; large enough for any frame of at most about 700 octets.
    pub detail: Str<16384>,
}

impl Default for Parsed {
    fn default() -> Self {
        Parsed {
            ok: 0,
            from_air: 0,
            from_ground: 0,
            on_ground: 0,
            response: 0,
            pf: 0,
            has_pos: 0,
            has_alt: 0,
            frame_kind: 0,
            dst_type: 0,
            src_type: 0,
            ns: -1,
            nr: -1,
            alt_ft: 0,
            acars_at: -1,
            dst: 0,
            src: 0,
            lat: 0.0,
            lon: 0.0,
            frame_name: Str::default(),
            proto: Str::default(),
            summary: Str::default(),
            pos_kind: Str::default(),
            detail: Str::default(),
        }
    }
}

/// Appends one text line (newline terminated) to the detail text.
pub fn line<const N: usize>(d: &mut Str<N>, f: impl FnOnce(&mut Buf)) {
    d.append(|w| {
        f(w);
        w.byte(b'\n');
    });
}

/// Upper case hex of at most `max` octets, "..." appended when there are more (`hexs` of the reference; max 24 unless given).
pub fn hexs(w: &mut Buf, p: &[u8], max: usize) {
    for &b in p.iter().take(max) {
        w.hex(b as u64, 2, true);
    }
    if p.len() > max {
        w.str("...");
    }
}

/// `%.*s`: at most `len` bytes, stopping at a NUL like C does.
pub fn cstr_n(w: &mut Buf, p: &[u8]) {
    for &c in p.iter().take_while(|&&c| c != 0) {
        w.byte(c);
    }
}

fn bcd(w: &mut Buf, p: &[u8], digits: usize, start_nibble: usize) {
    for i in 0..digits {
        let nib = start_nibble + i;
        let v = (p[nib / 2] >> (if nib % 2 != 0 { 0 } else { 4 })) & 0xF;
        w.byte(if v < 10 { b'0' + v } else { b'?' });
    }
}

/// COTP TPDU type name (ISO 8073 13.2).
fn cotp_name(c: u8) -> &'static str {
    match c & 0xF0 {
        0xE0 => "CR (connection request)",
        0xD0 => "CC (connection confirm)",
        0xF0 => "DT (data)",
        0x80 => "DR (disconnect request)",
        0xC0 => "DC (disconnect confirm)",
        0x60 => "AK (acknowledgement)",
        0x10 => "ED (expedited data)",
        0x20 => "EA (expedited ack)",
        0x70 => "ER (error)",
        _ => "unknown TPDU",
    }
}

/// CLNP (ISO 8473) header and what it carries: the PDU type, lifetime, segment length, the NSAP addresses and, for data PDUs, the COTP TPDU.
pub fn read_clnp(p: &[u8], m: &mut Parsed) {
    let n = p.len();
    if n < 9 {
        line(&mut m.detail, |w| {
            w.str("CLNP: cut short (");
            w.udec(n as u64);
            w.str(" octets)");
        });
        return;
    }
    let (li, ver, life, ty) = (p[1] as usize, p[2], p[3], p[4] & 0x1F);
    let seg_len = (p[5] as u32) << 8 | p[6] as u32;
    let tn = match ty {
        0x1C => "DT (data)",
        0x01 => "ER (error report)",
        0x1E => "ERQ (echo request)",
        0x1F => "ERP (echo reply)",
        _ => "unknown",
    };
    m.proto.set(b"CLNP");
    let flags = p[4];
    line(&mut m.detail, |w| {
        w.str("CLNP: ");
        w.str(tn);
        w.str(", version ");
        w.idec(ver as i64);
        w.str(", lifetime ");
        w.fixed(crate::ffi::cfn::format_f, 1, life as f64 * 0.5);
        w.str(" s, segment ");
        w.udec(seg_len as u64);
        w.str(" octets, header ");
        w.udec(li as u64);
        w.str(" octets");
        if flags & 0x80 != 0 {
            w.str(", segmented");
        }
        if flags & 0x20 != 0 {
            w.str(", error reports wanted");
        }
    });
    let mut o = 9usize;
    let (mut dst, mut src): (&[u8], &[u8]) = (&[], &[]);
    if o < n {
        let dl = p[o] as usize;
        o += 1;
        if o + dl <= n {
            dst = &p[o..o + dl];
            o += dl;
        }
    }
    if o < n {
        let sl = p[o] as usize;
        o += 1;
        if o + sl <= n {
            src = &p[o..o + sl];
        }
    }
    // the NSAP text is "hexs(p, n, 20)" of the reference: an empty address shows as "-"
    let nsap = |w: &mut Buf, a: &[u8]| {
        if a.is_empty() {
            w.byte(b'-');
        } else {
            hexs(w, a, 20);
        }
    };
    line(&mut m.detail, |w| {
        w.str("  destination NSAP ");
        nsap(w, dst);
    });
    line(&mut m.detail, |w| {
        w.str("  source NSAP      ");
        nsap(w, src);
    });
    m.summary.write(|w| {
        w.str("CLNP ");
        w.str(if ty == 0x1C { "DT" } else { tn });
    });
    if ty == 0x1C && li < n {
        let q = &p[li..];
        let qn = q.len();
        if qn >= 2 && (q[0] as usize) < qn {
            m.proto.set(b"COTP");
            let ul = qn - q[0] as usize - 1;
            line(&mut m.detail, |w| {
                w.str("COTP (ISO 8073): ");
                w.str(cotp_name(q[1]));
                w.str(", ");
                w.udec(ul as u64);
                w.str(" octets of user data");
            });
            let name = cotp_name(q[1]);
            m.summary.append(|w| {
                w.str(", COTP ");
                w.bytes(&name.as_bytes()[..2]);
            });
            if q[1] & 0xF0 == 0xF0 && qn > q[0] as usize + 1 {
                line(&mut m.detail, |w| {
                    w.str("  ATN application data (CPDLC, ADS-C, CM over ATN: ASN.1 PER, not decoded): ");
                    hexs(w, &q[q[0] as usize + 1..], 24);
                });
                m.summary.append(|w| w.str(", ATN application data"));
            }
        }
    }
}

/// Packet text for one of the named X.25 packets: summary "X.25 <name>, LCN <n>" and the detail line "X.25 (ISO 8208): <name>, logical channel <n>".
fn simple(m: &mut Parsed, name: &str, lcn: u32) {
    m.summary.write(|w| {
        w.str("X.25 ");
        w.str(name);
        w.str(", LCN ");
        w.udec(lcn as u64);
    });
    line(&mut m.detail, |w| {
        w.str("X.25 (ISO 8208): ");
        w.str(name);
        w.str(", logical channel ");
        w.udec(lcn as u64);
    });
}

/// X.25 (ISO 8208) packet: general format identifier, logical channel number, packet type and the contents of data, call, clear and control packets.
pub fn read_x25(p: &[u8], m: &mut Parsed) {
    let n = p.len();
    m.proto.set(b"X.25");
    if n < 3 {
        m.summary.set(b"X.25: cut short");
        line(&mut m.detail, |w| {
            w.str("X.25 (ISO 8208): ");
            w.udec(n as u64);
            w.str(" octets, too short");
        });
        return;
    }
    let gfi = p[0] >> 4;
    let lcn = ((p[0] & 0xF) as u32) << 8 | p[1] as u32;
    let pt = p[2];
    if pt & 1 == 0 {
        let (pr, ps, more) = (pt >> 5, (pt >> 1) & 7, (pt >> 4) & 1);
        m.summary.write(|w| {
            w.str("X.25 data, LCN ");
            w.udec(lcn as u64);
            w.str(", P(S) ");
            w.udec(ps as u64);
            w.str(" P(R) ");
            w.udec(pr as u64);
            if more != 0 {
                w.str(", more follows");
            }
        });
        line(&mut m.detail, |w| {
            w.str("X.25 (ISO 8208): data packet, GFI ");
            w.hex(gfi as u64, 1, true);
            w.str(", logical channel ");
            w.udec(lcn as u64);
            w.str(", P(S) ");
            w.udec(ps as u64);
            w.str(", P(R) ");
            w.udec(pr as u64);
            if more != 0 {
                w.str(", M bit set");
            }
            w.str(", ");
            w.udec((n - 3) as u64);
            w.str(" octets of user data");
        });
        let u = &p[3..];
        if u.is_empty() {
            return;
        }
        if u[0] == 0x81 {
            let s = m.summary;
            read_clnp(u, m);
            let inner = m.summary;
            m.summary.write(|w| {
                w.bytes(s.as_bytes());
                w.str(": ");
                w.bytes(inner.as_bytes());
            });
        } else if u[0] == 0x82 {
            m.proto.set(b"ES-IS");
            m.summary.append(|w| w.str(": ES-IS"));
            line(&mut m.detail, |w| {
                w.str("ES-IS (ISO 9542) routeing exchange, ");
                w.udec(u.len() as u64);
                w.str(" octets");
            });
        } else if u[0] == 0x85 {
            m.proto.set(b"IDRP");
            m.summary.append(|w| w.str(": IDRP"));
            line(&mut m.detail, |w| {
                w.str("IDRP (ISO 10747) inter-domain routeing, ");
                w.udec(u.len() as u64);
                w.str(" octets");
            });
        } else {
            m.summary.append(|w| w.str(": compressed CLNP header"));
            line(&mut m.detail, |w| {
                w.str("ICAO SNDCF compressed (LREF) CLNP header: ");
                hexs(w, u, 24);
            });
        }
        return;
    }
    match pt {
        0x0B | 0x0F => {
            let req = pt == 0x0B;
            let (mut called, mut calling): (([u8; 16], usize), ([u8; 16], usize)) = (([0; 16], 0), ([0; 16], 0));
            let mut o = 3usize;
            if n > 3 {
                let lcalling = (p[3] >> 4) as usize;
                let lcalled = (p[3] & 0xF) as usize;
                o = 4;
                let bytes = (lcalling + lcalled + 1) / 2;
                if o + bytes <= n {
                    let mut b = Buf::new(&mut called.0);
                    bcd(&mut b, &p[o..], lcalled, 0);
                    called.1 = b.len();
                    let mut b = Buf::new(&mut calling.0);
                    bcd(&mut b, &p[o..], lcalling, lcalled);
                    calling.1 = b.len();
                    o += bytes;
                }
            }
            let mut fac_len: i64 = -1;
            if o < n {
                fac_len = p[o] as i64;
                o += 1 + fac_len as usize;
            }
            let name = if req { "call request" } else { "call accepted" };
            simple(m, name, lcn);
            line(&mut m.detail, |w| {
                w.str("  called DTE ");
                if called.1 == 0 {
                    w.byte(b'-');
                } else {
                    w.bytes(&called.0[..called.1]);
                }
                w.str(", calling DTE ");
                if calling.1 == 0 {
                    w.byte(b'-');
                } else {
                    w.bytes(&calling.0[..calling.1]);
                }
                w.str(", facilities ");
                w.idec(fac_len.max(0));
                w.str(" octets");
            });
            if o < n {
                line(&mut m.detail, |w| {
                    w.str("  call user data: ");
                    hexs(w, &p[o..], 24);
                    if p[o] == 0xC1 {
                        w.str(" (ICAO SNDCF)");
                    }
                });
            }
        }
        0x13 | 0x17 => {
            let what = if pt == 0x13 { "request" } else { "confirm" };
            m.summary.write(|w| {
                w.str("X.25 clear ");
                w.str(what);
                w.str(", LCN ");
                w.udec(lcn as u64);
            });
            line(&mut m.detail, |w| {
                w.str("X.25 (ISO 8208): clear ");
                w.str(if pt == 0x13 { "request" } else { "confirmation" });
                w.str(", logical channel ");
                w.udec(lcn as u64);
            });
            if pt == 0x13 && n >= 5 {
                line(&mut m.detail, |w| {
                    w.str("  cause ");
                    w.udec(p[3] as u64);
                    w.str(", diagnostic ");
                    w.udec(p[4] as u64);
                });
            }
        }
        0x1B | 0x1F | 0xFB | 0xFF | 0x23 | 0x27 | 0xF1 => {
            let name = match pt {
                0x1B => "reset request",
                0x1F => "reset confirmation",
                0xFB => "restart request",
                0xFF => "restart confirmation",
                0x23 => "interrupt",
                0x27 => "interrupt confirmation",
                _ => "diagnostic",
            };
            simple(m, name, lcn);
        }
        _ => {
            let low = pt & 0x1F;
            if low == 0x01 || low == 0x05 || low == 0x09 {
                let nm = if low == 0x01 {
                    "RR"
                } else if low == 0x05 {
                    "RNR"
                } else {
                    "REJ"
                };
                m.summary.write(|w| {
                    w.str("X.25 ");
                    w.str(nm);
                    w.str(", LCN ");
                    w.udec(lcn as u64);
                    w.str(", P(R) ");
                    w.udec((pt >> 5) as u64);
                });
                line(&mut m.detail, |w| {
                    w.str("X.25 (ISO 8208): ");
                    w.str(nm);
                    w.str(", logical channel ");
                    w.udec(lcn as u64);
                    w.str(", P(R) ");
                    w.udec((pt >> 5) as u64);
                });
                return;
            }
            m.summary.write(|w| {
                w.str("X.25 packet type ");
                w.hex(pt as u64, 2, true);
                w.str(", LCN ");
                w.udec(lcn as u64);
            });
            line(&mut m.detail, |w| {
                w.str("X.25 (ISO 8208): packet type ");
                w.hex(pt as u64, 2, true);
                w.str(", logical channel ");
                w.udec(lcn as u64);
                w.str(", ");
                w.udec(n as u64);
                w.str(" octets");
            });
        }
    }
}
