// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! LoRa APRS and MeshCom 4 packet layer. LoRa APRS: payload = "<" 0xFF 0x01 + a TNC2 text packet "SRC>DEST,PATH:info" (lora_utils.cpp of
//! richonguzman/LoRa_APRS_iGate; a trailing NUL, CR or LF is dropped). MeshCom: type byte (':' text, '!' position, '@' HEY, 'A' ack), message id (4 bytes
//! little endian), flags / max hop byte, "SRCPATH>DEST" + type + payload + 0x00, hardware id, modulation | country << 4, 16 bit byte sum, firmware version and
//! trailer (decodeAPRS / encodeAPRS in icssw-org/MeshCom-Firmware src/aprs_functions.cpp, handleACK in lora_functions.cpp, ack_functions.h). This file only
//! splits and checks the frames and builds the text of the parts that need no APRS parser; the APRS information field itself is parsed by `aprs::parse` in C++.
use crate::abi::{S128, S2048, S256, S64};
use onair_common::text::Buf;

/// ASCII letters and digits ("C" locale `isalnum`) and the dash.
fn call_char(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'-'
}

/// A station call as both firmwares accept it, loosely: 1..20 letters, digits and '-'.
pub fn plausible_call(s: &[u8]) -> bool {
    !s.is_empty() && s.len() <= 20 && s.iter().all(|&c| call_char(c))
}

/// Printable ASCII kept (not the backslash), other bytes as `\xNN`.
pub fn printable(w: &mut Buf, p: &[u8]) {
    for &c in p {
        if (0x20..0x7F).contains(&c) && c != b'\\' {
            w.byte(c);
        } else {
            w.str("\\x");
            w.hex(c as u64, 2, false);
        }
    }
}

/// Invalid UTF-8 and control characters become '?' (comments may carry UTF-8; broken bytes must not reach the interface).
pub fn clean_text(w: &mut Buf, s: &[u8]) {
    let mut i = 0usize;
    while i < s.len() {
        let c = s[i];
        if c < 0x80 {
            w.byte(if c < 0x20 || c == 0x7F { b'?' } else { c });
            i += 1;
            continue;
        }
        let len = if c & 0xE0 == 0xC0 {
            2
        } else if c & 0xF0 == 0xE0 {
            3
        } else if c & 0xF8 == 0xF0 {
            4
        } else {
            0
        };
        let mut ok = len > 0 && i + len <= s.len() && !(c == 0xC0 || c == 0xC1 || c > 0xF4);
        let mut k = 1;
        while ok && k < len {
            ok = s[i + k] & 0xC0 == 0x80;
            k += 1;
        }
        if !ok {
            w.byte(b'?');
            i += 1;
            continue;
        }
        w.bytes(&s[i..i + len]);
        i += len;
    }
}

/// The result of splitting a LoRa APRS frame.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LoraAprs {
    /// 1 when the frame is a plausible TNC2 packet.
    pub ok: u8,
    pub note: S128,
    /// The payload as printable text.
    pub raw: S2048,
    pub src: S64,
    pub dest: S64,
    pub path: S256,
    pub info: S256,
}

/// Splits a LoRa APRS frame into source, destination, path and information field and checks the calls (the note says what is wrong).
pub fn lora_aprs(p: &[u8]) -> LoraAprs {
    let mut r = LoraAprs::default();
    r.raw.write(|w| printable(w, p));
    if p.len() < 3 || p[0] != 0x3C || p[1] != 0xFF || p[2] != 0x01 {
        r.note.set(b"no LoRa APRS prefix");
        return r;
    }
    let mut s = &p[3..];
    while let Some(&l) = s.last() {
        if l == 0 || l == b'\r' || l == b'\n' {
            s = &s[..s.len() - 1];
        } else {
            break;
        }
    }
    let gt = s.iter().position(|&c| c == b'>');
    let colon = s.iter().position(|&c| c == b':');
    let (gt, colon) = match (gt, colon) {
        (Some(g), Some(c)) if g != 0 && c >= g + 2 => (g, c),
        _ => {
            r.note.set(b"not a TNC2 packet");
            return r;
        }
    };
    let src = &s[..gt];
    let hdr = &s[gt + 1..colon];
    let (dest, path) = match hdr.iter().position(|&c| c == b',') {
        Some(c) => (&hdr[..c], &hdr[c + 1..]),
        None => (hdr, &hdr[hdr.len()..]),
    };
    if !plausible_call(src) || !plausible_call(dest) {
        r.note.set(b"bad call in the TNC2 header");
        return r;
    }
    if !path.iter().all(|&c| call_char(c) || c == b',' || c == b'*') {
        r.note.set(b"bad path in the TNC2 header");
        return r;
    }
    r.ok = 1;
    r.src.set(src);
    r.dest.set(dest);
    r.path.set(path);
    r.info.set(&s[colon + 1..]);
    r
}

/// Kinds of MeshCom frame.
pub const MC_ACK: u8 = 0;
/// Text message (':').
pub const MC_TEXT: u8 = 1;
/// Position ('!').
pub const MC_POSITION: u8 = 2;
/// HEY beacon ('@').
pub const MC_HEY: u8 = 3;

/// The result of decoding a MeshCom frame.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MeshCom {
    /// 1 when the frame parsed; `kind` is one of `MC_*`.
    pub ok: u8,
    pub kind: u8,
    pub has_aprs: u8,
    pub decrypted: u8,
    pub hop_limit: i32,
    pub battery_pct: i32,
    pub packet_id: u32,
    pub note: S128,
    pub raw: S2048,
    /// Packet type text ("TEXT", "POSITION", "HEY", "ACK").
    pub type_name: S64,
    /// Detail line (complete for ACK, TEXT and HEY; for POSITION only `tail` is set).
    pub detail: S2048,
    /// ", HW 4, MOD 3, country 0, FW 35, server..." summary of the trailer and flags (without the parentheses).
    pub tail: S256,
    /// Sender, relays after the sender, destination field, the call part of the destination and the APRS info (type character + payload).
    pub src: S64,
    pub aprs_path: S256,
    pub aprs_dest: S256,
    pub dest_call: S256,
    pub aprs_info: S256,
    /// Text message: channel, cleaned text.
    pub channel: S256,
    pub text: S256,
}

impl Default for MeshCom {
    fn default() -> Self {
        MeshCom {
            ok: 0,
            kind: 0,
            has_aprs: 0,
            decrypted: 0,
            hop_limit: -1,
            battery_pct: -1,
            packet_id: 0,
            note: Default::default(),
            raw: Default::default(),
            type_name: Default::default(),
            detail: Default::default(),
            tail: Default::default(),
            src: Default::default(),
            aprs_path: Default::default(),
            aprs_dest: Default::default(),
            dest_call: Default::default(),
            aprs_info: Default::default(),
            channel: Default::default(),
            text: Default::default(),
        }
    }
}

/// "/B=085" in a MeshCom position (aprsExtractTag 'B', an integer of at most three digits); -1 when absent.
fn battery(s: &[u8]) -> i32 {
    let p = match s.windows(3).position(|w| w == b"/B=") {
        Some(p) => p,
        None => return -1,
    };
    let (mut i, mut v, mut nd) = (p + 3, 0i32, 0);
    while i < s.len() && s[i].is_ascii_digit() && nd < 3 {
        v = v * 10 + (s[i] - b'0') as i32;
        i += 1;
        nd += 1;
    }
    if nd != 0 {
        v
    } else {
        -1
    }
}

fn le32(p: &[u8]) -> u32 {
    u32::from_le_bytes([p[0], p[1], p[2], p[3]])
}

/// Decodes a MeshCom frame (see the module header); the note says what is wrong when `ok` stays 0.
pub fn meshcom(p: &[u8]) -> MeshCom {
    let n = p.len();
    let mut r = MeshCom::default();
    r.raw.write(|w| printable(w, p));
    if n == 0 {
        return r;
    }
    let kind = p[0];
    if kind == 0x41 {
        // ack_functions.h isPlausibleAckFrame: 12 bytes or more, byte 5 = 0x80 | remaining hops (at most MAX_HOP_LIMIT 7). Bytes 10 / 11 are 0x01 0x00
        // in nearly every real ack; the firmware does not check them, we do so that random data is not taken for an ack.
        if n < 12 || (p[5] & 0x80) != 0x80 || (p[5] & 0x7F) > 7 || p[10] != 0x01 || p[11] != 0x00 {
            r.note.set(b"not a MeshCom ack");
            return r;
        }
        r.ok = 1;
        r.kind = MC_ACK;
        r.type_name.set(b"ACK");
        r.packet_id = le32(&p[1..]);
        let acked = le32(&p[6..]);
        r.hop_limit = (p[5] & 0x7F) as i32;
        r.detail.write(|w| {
            w.str("ack of ");
            w.hex(acked as u64, 8, true);
        });
        r.decrypted = 1;
        return r;
    }
    if kind != b':' && kind != b'!' && kind != b'@' {
        r.note.set(b"not a MeshCom packet");
        return r;
    }
    if n < 16 {
        r.note.set(b"short MeshCom packet");
        return r;
    }
    let printable_char = |c: u8| (0x20..=0x7E).contains(&c);
    let mut i = 6usize;
    while i < n && i - 6 < 120 && p[i] != b'>' && printable_char(p[i]) {
        i += 1;
    }
    let src_path = &p[6..i];
    if i >= n || p[i] != b'>' {
        r.note.set(b"MeshCom: no '>' after the source");
        return r;
    }
    i += 1;
    let d0 = i;
    while i < n && i - d0 < 120 && p[i] != kind && printable_char(p[i]) {
        i += 1;
    }
    let dest = &p[d0..i];
    if i >= n || p[i] != kind {
        r.note.set(b"MeshCom: no type character after the destination");
        return r;
    }
    i += 1;
    let pay0 = i;
    while i < n && p[i] != 0 {
        i += 1;
    }
    let payload = &p[pay0..i];
    if i >= n {
        r.note.set(b"MeshCom: payload not ended");
        return r;
    }
    i += 1;
    if i + 4 > n {
        r.note.set(b"MeshCom: trailer cut off");
        return r;
    }
    let (hw, modu) = (p[i], p[i + 1]);
    let sum: u32 = p[..i + 2].iter().map(|&b| b as u32).sum();
    let fcs = (p[i + 2] as u32) << 8 | p[i + 3] as u32;
    if sum & 0xFFFF != fcs {
        r.note.set(b"MeshCom: checksum wrong");
        return r;
    }
    let src_len = src_path.iter().position(|&c| c == b',').unwrap_or(src_path.len());
    let src = &src_path[..src_len];
    if !plausible_call(src) {
        r.note.set(b"MeshCom: bad source call");
        return r;
    }
    let dest_call = match dest.iter().rposition(|&c| c == b',') {
        Some(c) => &dest[c + 1..],
        None => dest,
    };
    let j = i + 4;
    let fw: i32 = if j < n { p[j] as i32 } else { -1 };
    r.ok = 1;
    r.has_aprs = 1;
    r.src.set(src);
    r.aprs_path.set(if src_path.len() > src.len() { &src_path[src.len() + 1..] } else { &[] });
    r.aprs_dest.set(dest);
    r.dest_call.set(dest_call);
    r.aprs_info.write(|w| {
        w.byte(kind);
        w.bytes(payload);
    });
    r.packet_id = le32(&p[1..]);
    r.hop_limit = (p[5] & 0x0F) as i32;
    r.decrypted = 1;
    r.tail.write(|w| {
        w.str("HW ");
        w.udec(hw as u64);
        w.str(", MOD ");
        w.udec((modu & 0x0F) as u64);
        w.str(", country ");
        w.udec((modu >> 4) as u64);
        if fw >= 0 {
            w.str(", FW ");
            w.idec(fw as i64);
        }
        if p[5] & 0x80 != 0 {
            w.str(", server");
        }
        if p[5] & 0x40 != 0 {
            w.str(", track");
        }
        if p[5] & 0x20 != 0 {
            w.str(", app offline");
        }
        if p[5] & 0x10 != 0 {
            w.str(", mesh");
        }
    });
    let tail = r.tail;
    let head = &payload[..payload.len().min(60)];
    match kind {
        b':' => {
            r.kind = MC_TEXT;
            r.type_name.set(b"TEXT");
            r.channel.write(|w| {
                if dest_call == b"*" {
                    w.str("MeshCom all");
                } else {
                    w.str("MeshCom ");
                    w.bytes(dest_call);
                }
            });
            r.text.write(|w| clean_text(w, payload));
            r.detail.write(|w| {
                clean_text(w, head);
                w.str(" (");
                w.bytes(tail.as_bytes());
                w.byte(b')');
            });
        }
        b'!' => {
            r.kind = MC_POSITION;
            r.type_name.set(b"POSITION");
            r.battery_pct = battery(payload);
        }
        _ => {
            r.kind = MC_HEY;
            r.type_name.set(b"HEY");
            r.detail.write(|w| {
                clean_text(w, head);
                w.str(" (");
                w.bytes(tail.as_bytes());
                w.byte(b')');
            });
        }
    }
    r
}
