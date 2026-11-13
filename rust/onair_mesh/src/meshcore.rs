// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! MeshCore packets: the header byte (route type, payload type, version), transport codes, the path length byte (hash size in the top two bits), the path, and
//! the payload layouts that need no key: ADVERT (public key, time, signature, app data), ACK, TRACE, CONTROL, ANON_REQ and the headers of the encrypted types,
//! plus the plain text of a decrypted GRP_TXT / GRP_DATA. Sources: meshcore-dev/MeshCore src/Packet.h and Packet.cpp (header byte, transport codes, path length
//! byte), src/Mesh.cpp (ADVERT signature message, GRP_TXT / GRP_DATA handling, TRACE layout), src/helpers/BaseChatMesh.cpp (group text layout),
//! src/helpers/AdvertDataHelpers.{h,cpp} (advert app data), docs/packet_format.md. The AES / HMAC / Ed25519 steps stay in C++.
use crate::abi::{Message, Node, S128, S2048, S256, S64};
use crate::ffi::cfn::format_f;
use onair_common::text::Buf;

/// The packet type name, or `None` for the types without one.
fn type_name(t: u8) -> Option<&'static str> {
    Some(match t {
        0 => "REQ",
        1 => "RESPONSE",
        2 => "TXT_MSG",
        3 => "ACK",
        4 => "ADVERT",
        5 => "GRP_TXT",
        6 => "GRP_DATA",
        7 => "ANON_REQ",
        8 => "PATH",
        9 => "TRACE",
        10 => "MULTIPART",
        11 => "CONTROL",
        15 => "RAW_CUSTOM",
        _ => return None,
    })
}

/// The node role named by the low four bits of the advert flags.
fn adv_role(t: u8) -> &'static str {
    match t {
        1 => "Chat",
        2 => "Repeater",
        3 => "Room server",
        4 => "Sensor",
        _ => "Unknown",
    }
}

/// Text up to the first NUL with control characters replaced by blanks.
fn clean_line(w: &mut Buf, p: &[u8]) {
    for &c in p.iter().take_while(|&&c| c != 0) {
        w.byte(if c < 0x20 { b' ' } else { c });
    }
}

fn le32(p: &[u8]) -> u32 {
    u32::from_le_bytes([p[0], p[1], p[2], p[3]])
}

/// A header that passed the length checks (Packet.cpp readFrom): offsets of the path and the payload in the packet.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Parsed {
    /// Route type 0..3 (flood / direct, with or without transport codes), payload type 0..15, payload version 0..3.
    pub route: u8,
    pub kind: u8,
    pub ver: u8,
    /// Number of path hashes and the bytes per hash (1, 2 or 3).
    pub hcnt: u8,
    pub hsz: u8,
    /// Offset of the path and of the payload; payload length.
    pub path_off: u16,
    pub pay_off: u16,
    pub m: u16,
    /// Type name (`TYPE_<n>` when unnamed), the path hashes in hex separated by commas, and the hop text (", 3 hops", ", direct from sender" or empty).
    pub type_name: S64,
    pub path: S256,
    pub via: S64,
}

/// Checks and splits the packet. `Err` is the note for a packet that cannot be a MeshCore packet; the notes are those of the C++ reference.
pub fn parse_header(p: &[u8]) -> Result<Parsed, &'static [u8]> {
    let n = p.len();
    if n < 3 {
        return Err(b"too short for a MeshCore packet");
    }
    let header = p[0];
    let route = header & 3;
    let kind = (header >> 2) & 15;
    let ver = header >> 6;
    let mut i = 1usize;
    if route == 0 || route == 3 {
        if n < 6 {
            return Err(b"truncated transport codes");
        }
        i += 4;
    }
    let pl = p[i];
    i += 1;
    let hsz = (pl >> 6) as usize + 1;
    let hcnt = (pl & 63) as usize;
    if hsz == 4 || hcnt * hsz > 64 {
        return Err(b"bad path length byte");
    }
    let path_bytes = hcnt * hsz;
    if i + path_bytes >= n {
        return Err(b"no payload");
    }
    let path_off = i;
    i += path_bytes;
    let m = n - i;
    if m > 184 {
        return Err(b"payload longer than 184 bytes");
    }
    let mut out = Parsed {
        route,
        kind,
        ver,
        hcnt: hcnt as u8,
        hsz: hsz as u8,
        path_off: path_off as u16,
        pay_off: i as u16,
        m: m as u16,
        type_name: S64::default(),
        path: S256::default(),
        via: S64::default(),
    };
    out.type_name.write(|w| match type_name(kind) {
        Some(t) => w.str(t),
        None => {
            w.str("TYPE_");
            w.udec(kind as u64);
        }
    });
    out.path.write(|w| {
        for k in 0..hcnt {
            if k != 0 {
                w.byte(b',');
            }
            for &b in &p[path_off + k * hsz..path_off + (k + 1) * hsz] {
                w.hex(b as u64, 2, false);
            }
        }
    });
    let flood = route == 0 || route == 1;
    out.via.write(|w| {
        if hcnt != 0 {
            w.str(", ");
            w.udec(hcnt as u64);
            w.str(if hcnt == 1 { " hop" } else { " hops" });
        } else if flood {
            w.str(", direct from sender");
        }
    });
    Ok(out)
}

/// What decoding the key-free part of a packet says about it.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Out {
    /// 1 when the payload is a group or end-to-end encrypted type that still needs the key loop (the fields below hold what is known before it).
    pub need_crypto: u8,
    /// Packet info fields: decrypted flag, "clear the path text" flag (trace), a node update present.
    pub decrypted: u8,
    pub clear_path: u8,
    pub has_node: u8,
    pub from: S64,
    pub to: S64,
    pub note: S128,
    pub detail: S2048,
    pub node: Node,
}

fn hex_into(w: &mut Buf, p: &[u8]) {
    for &b in p {
        w.hex(b as u64, 2, false);
    }
}

/// The text of `to X from Y` plus the hop text, used for packets that cannot be read without the key.
fn to_from(o: &mut Out, via: &[u8]) {
    let (to, from) = (o.to, o.from);
    o.detail.write(|w| {
        w.str("to ");
        w.bytes(to.as_bytes());
        w.str(" from ");
        w.bytes(from.as_bytes());
        w.bytes(via);
    });
}

/// Decodes everything that needs no key: `pay` is the payload of the parsed packet, `path` its path bytes, `sig_ok` the result of the Ed25519 check of an
/// advert (done by the caller over public key, time and app data; ignored for other types).
pub fn decode(h: &Parsed, path: &[u8], pay: &[u8], sig_ok: bool) -> Out {
    let mut o = Out::default();
    let m = pay.len();
    let via = h.via;
    let via = via.as_bytes();
    match h.kind {
        4 => {
            if m < 32 + 4 + 64 + 1 {
                o.note.set(b"incomplete advert");
                return o;
            }
            let ts = le32(&pay[32..]);
            let app = &pay[100..];
            let al = app.len().min(32); // Mesh.cpp clamps to MAX_ADVERT_DATA_SIZE
            let app = &app[..al];
            o.from.write(|w| hex_into(w, &pay[..6]));
            o.decrypted = 1; // adverts are not encrypted
            let flags = app[0];
            let mut k = 1usize;
            let mut u = Node::default();
            u.role.set(adv_role(flags & 15).as_bytes());
            u.public_key.write(|w| hex_into(w, &pay[..32]));
            u.signature_checked = 1;
            u.signature_ok = sig_ok as u8;
            u.position_time = ts;
            let mut bad = false;
            if flags & 0x10 != 0 {
                if k + 8 > al {
                    bad = true;
                } else {
                    let la = le32(&app[k..]) as i32;
                    let lo = le32(&app[k + 4..]) as i32;
                    k += 8;
                    if la != 0 || lo != 0 {
                        u.has_position = 1;
                        u.lat = la as f64 / 1e6;
                        u.lon = lo as f64 / 1e6;
                    }
                }
            }
            if !bad && flags & 0x20 != 0 {
                if k + 2 > al {
                    bad = true;
                } else {
                    k += 2;
                }
            }
            if !bad && flags & 0x40 != 0 {
                if k + 2 > al {
                    bad = true;
                } else {
                    k += 2;
                }
            }
            if !bad && flags & 0x80 != 0 {
                u.long_name.write(|w| clean_line(w, &app[k..]));
            }
            if bad {
                o.note.set(b"incomplete advert data");
                return o;
            }
            let name = u.long_name;
            let role = u.role;
            o.detail.write(|w| {
                if name.len == 0 {
                    w.str("(no name)");
                } else {
                    w.bytes(name.as_bytes());
                }
                w.str(", ");
                w.bytes(role.as_bytes());
                if u.has_position != 0 {
                    w.str(", ");
                    w.fixed(format_f, 5, u.lat);
                    w.str(", ");
                    w.fixed(format_f, 5, u.lon);
                }
                w.str(if sig_ok { ", signature ok" } else { ", signature NOT valid" });
                w.bytes(via);
            });
            o.node = u;
            o.has_node = 1;
            if !sig_ok {
                o.note.set(b"signature not valid");
            }
        }
        5 | 6 => {
            if m < 4 {
                o.note.set(b"incomplete group packet");
            } else {
                o.need_crypto = 1;
            }
        }
        0 | 1 | 2 | 8 => {
            if m < 4 {
                o.note.set(b"incomplete packet");
            } else {
                o.to.write(|w| hex_into(w, &pay[..1]));
                o.from.write(|w| hex_into(w, &pay[1..2]));
                o.need_crypto = 1;
            }
        }
        7 => {
            if m < 1 + 32 + 2 {
                o.note.set(b"incomplete packet");
            } else {
                o.to.write(|w| hex_into(w, &pay[..1]));
                o.from.write(|w| hex_into(w, &pay[1..7]));
                o.note.set(b"encrypted (direct)");
                to_from(&mut o, via);
            }
        }
        3 => {
            if m < 4 {
                o.note.set(b"incomplete ack");
            } else {
                o.decrypted = 1;
                o.detail.write(|w| {
                    w.str("ack ");
                    w.hex(le32(pay) as u64, 8, false);
                    w.bytes(via);
                });
            }
        }
        9 => {
            if m < 9 {
                o.note.set(b"incomplete trace");
            } else {
                o.decrypted = 1;
                let sz = 1usize << (pay[8] & 3);
                let hops = (m - 9) / sz;
                o.detail.write(|w| {
                    w.str("trace ");
                    w.hex(le32(pay) as u64, 8, false);
                    w.str(", ");
                    w.idec(hops as i64);
                    w.str(if hops == 1 { " node" } else { " nodes" });
                    if h.hcnt != 0 {
                        // each entry of the path is an SNR in quarter dB
                        w.str(", SNR");
                        for &b in path.iter().take(h.hcnt as usize) {
                            w.byte(b' ');
                            w.fixed(format_f, 2, (b as i8) as f64 / 4.0);
                        }
                        w.str(" dB");
                    }
                });
                o.clear_path = 1;
            }
        }
        11 => {
            o.decrypted = 1;
            o.detail.write(|w| {
                w.str("control, first byte 0x");
                w.hex(pay[0] as u64, 2, false);
            });
        }
        _ => o.detail.write(|w| {
            w.udec(m as u64);
            w.str(" bytes");
        }),
    }
    o
}

/// What a decrypted GRP_TXT / GRP_DATA says (BaseChatMesh.cpp: time, flags byte, "name: text").
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct GroupOut {
    /// Packet type text ("GRP_TXT" / "GRP_DATA").
    pub type_name: S64,
    /// Note: empty when fully decoded, else why not.
    pub note: S128,
    pub detail: S2048,
    /// A text message was found (its sender and the channel go to the packet's from / to).
    pub has_message: u8,
    pub message: Message,
}

/// Interprets the decrypted bytes `d` of a group packet of `kind` (5 text, 6 data) that travelled `hcnt` hops; `via` is the hop text.
pub fn group_plain(kind: u8, hcnt: u8, via: &[u8], d: &[u8]) -> GroupOut {
    let mut g = GroupOut::default();
    g.type_name.set(if kind == 6 { b"GRP_DATA" } else { b"GRP_TXT" });
    if kind == 5 {
        if d.len() < 5 {
            g.note.set(b"short group text");
            return g;
        }
        let tt = d[4] >> 2;
        if tt != 0 {
            g.note.write(|w| {
                w.str("unsupported text type ");
                w.udec(tt as u64);
            });
            return g;
        }
        let mut line = S256::default();
        line.write(|w| clean_line(w, &d[5..]));
        let l = line.as_bytes();
        let mut t = Message::default();
        t.sender_time = le32(d);
        t.hops = hcnt as i32;
        match l.windows(2).position(|x| x == b": ") {
            Some(sep) => {
                t.from.set(&l[..sep]);
                t.text.set(&l[sep + 2..]);
            }
            None => t.text.set(l),
        }
        let (from, text) = (t.from, t.text);
        g.detail.write(|w| {
            if from.len != 0 {
                w.bytes(from.as_bytes());
                w.str(": ");
            }
            w.bytes(text.as_bytes());
            w.bytes(via);
        });
        g.has_message = 1;
        g.message = t;
    } else {
        if d.len() < 3 {
            g.note.set(b"short group data");
            return g;
        }
        let dt = d[0] as u32 | (d[1] as u32) << 8;
        g.detail.write(|w| {
            w.str("data type 0x");
            w.hex(dt as u64, 4, false);
            w.str(", ");
            w.udec(d[2] as u64);
            w.str(" bytes");
            w.bytes(via);
        });
    }
    g
}

/// The note when no channel decrypted a group packet: `any` says that a channel with the same hash exists (so the MAC was wrong).
pub fn group_fail_note(hash: u8, any: bool) -> S128 {
    let mut s = S128::default();
    s.write(|w| {
        if any {
            w.str("MAC mismatch (wrong key or damaged)");
        } else {
            w.str("unknown channel 0x");
            w.hex(hash as u64, 2, false);
        }
    });
    s
}
