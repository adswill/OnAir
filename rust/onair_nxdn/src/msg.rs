// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! NXDN layer 3 messages (NXDN TS 1-D): building and parsing the 72 / 80 / 147-bit message bodies (type byte, call type, option, source and
//! destination ids, cipher, status, short data text, site / service info), the type and call type names, and the AMBE+2 voice frame bit order.

/// VCALL: voice call set-up.
pub const VCALL: i32 = 0x01;
/// TX_REL: transmission release.
pub const TX_REL: i32 = 0x08;
/// IDLE.
pub const IDLE: i32 = 0x10;
/// SITE_INFO (control channel).
pub const SITE_INFO: i32 = 0x18;
/// SRV_INFO (control channel).
pub const SRV_INFO: i32 = 0x19;
/// STAT_REQ: status message.
pub const STAT_REQ: i32 = 0x32;
/// SDCALL_HEADER: short data call header.
pub const SD_HEADER: i32 = 0x38;
/// SDCALL_DATA: short data block.
pub const SD_DATA: i32 = 0x39;

/// A layer 3 message; fields not used by the type keep their defaults (`call_type` 1).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Msg {
    /// Message type (6 bits), -1 before parsing.
    pub typ: i32,
    /// 0 broadcast, 1 group, 4 individual, 6 interconnect.
    pub call_type: i32,
    /// Voice call option (5 bits).
    pub option: i32,
    /// Source radio unit id.
    pub src: i32,
    /// Destination (talkgroup or unit).
    pub dst: i32,
    /// Cipher type (2 bits).
    pub cipher: i32,
    /// Key id (6 bits).
    pub key_id: i32,
    /// STAT_REQ status.
    pub status: i32,
    /// Short data block number.
    pub block: i32,
    /// Short data block count (header).
    pub blocks: i32,
    /// Short data text bytes, printable ASCII, zeros skipped.
    pub text: [u8; 8],
    /// Number of valid bytes in `text`.
    pub text_len: usize,
    /// SITE_INFO / SRV_INFO 24-bit location id.
    pub location: u32,
    /// Service information bits.
    pub service: i32,
    /// Channel (SITE_INFO).
    pub channel: i32,
}

impl Default for Msg {
    fn default() -> Self {
        Msg {
            typ: -1,
            call_type: 1,
            option: 0,
            src: 0,
            dst: 0,
            cipher: 0,
            key_id: 0,
            status: 0,
            block: 0,
            blocks: 0,
            text: [0; 8],
            text_len: 0,
            location: 0,
            service: 0,
            channel: 0,
        }
    }
}

/// MSB-first bit writer over a caller buffer: counts every bit, stores only those that fit.
struct W<'a> {
    b: &'a mut [u8],
    n: usize,
}
impl W<'_> {
    /// Appends the low `w` bits of `v`.
    fn put(&mut self, v: u32, w: u32) {
        for i in (0..w).rev() {
            if self.n < self.b.len() {
                self.b[self.n] = ((v >> i) & 1) as u8;
            }
            self.n += 1;
        }
    }
}

/// MSB-first bit reader; bits past the end read as 0.
struct R<'a> {
    b: &'a [u8],
    p: usize,
}
impl R<'_> {
    /// Reads `w` bits.
    fn get(&mut self, w: u32) -> u32 {
        let mut v = 0u32;
        for _ in 0..w {
            v = (v << 1) | if self.p < self.b.len() { (self.b[self.p] & 1) as u32 } else { 0 };
            self.p += 1;
        }
        v
    }
}

/// Writes the message into `bits` (cleared first, one bit per byte); returns the bit count of the message (also when it did not all fit).
pub fn build(m: &Msg, bits: &mut [u8]) -> usize {
    bits.iter_mut().for_each(|x| *x = 0);
    let mut w = W { b: bits, n: 0 };
    w.put(m.typ as u32 & 0x3F, 8);
    match m.typ {
        VCALL | TX_REL => {
            w.put(m.call_type as u32, 3);
            w.put(m.option as u32, 5);
            w.put(m.src as u32, 16);
            w.put(m.dst as u32, 16);
            w.put(m.cipher as u32, 2);
            w.put(m.key_id as u32, 6);
        }
        STAT_REQ => {
            w.put(m.call_type as u32, 3);
            w.put(m.option as u32, 5);
            w.put(m.src as u32, 16);
            w.put(m.dst as u32, 16);
            w.put(m.status as u32, 8);
        }
        SD_HEADER => {
            w.put(m.call_type as u32, 3);
            w.put(m.option as u32, 5);
            w.put(m.src as u32, 16);
            w.put(m.dst as u32, 16);
            w.put(m.blocks as u32, 8);
        }
        SD_DATA => {
            w.put(m.block as u32, 8);
            for i in 0..8 {
                w.put(if i < m.text_len { m.text[i] as u32 } else { 0 }, 8);
            }
        }
        SITE_INFO => {
            w.put(m.location, 24);
            w.put(m.channel as u32, 16);
            w.put(m.service as u32, 16);
        }
        SRV_INFO => {
            w.put(m.location, 24);
            w.put(m.service as u32, 16);
        }
        _ => {}
    }
    w.n
}

/// Parses a message from `bits` (one bit per byte) into `m` (reset first; the type is set even when the body is too short).
/// False when the body is shorter than its type needs; unknown types parse as the type alone.
pub fn parse(bits: &[u8], m: &mut Msg) -> bool {
    let n = bits.len();
    let mut r = R { b: bits, p: 0 };
    *m = Msg { typ: (r.get(8) & 0x3F) as i32, ..Msg::default() };
    match m.typ {
        VCALL | TX_REL => {
            if n < 56 {
                return false;
            }
            m.call_type = r.get(3) as i32;
            m.option = r.get(5) as i32;
            m.src = r.get(16) as i32;
            m.dst = r.get(16) as i32;
            m.cipher = r.get(2) as i32;
            m.key_id = r.get(6) as i32;
        }
        STAT_REQ => {
            if n < 56 {
                return false;
            }
            m.call_type = r.get(3) as i32;
            m.option = r.get(5) as i32;
            m.src = r.get(16) as i32;
            m.dst = r.get(16) as i32;
            m.status = r.get(8) as i32;
        }
        SD_HEADER => {
            if n < 56 {
                return false;
            }
            m.call_type = r.get(3) as i32;
            m.option = r.get(5) as i32;
            m.src = r.get(16) as i32;
            m.dst = r.get(16) as i32;
            m.blocks = r.get(8) as i32;
        }
        SD_DATA => {
            if n < 80 {
                return false;
            }
            m.block = r.get(8) as i32;
            for _ in 0..8 {
                let ch = r.get(8) as u8;
                if ch != 0 {
                    m.text[m.text_len] = if (32..127).contains(&ch) { ch } else { b'.' };
                    m.text_len += 1;
                }
            }
        }
        SITE_INFO => {
            if n < 64 {
                return false;
            }
            m.location = r.get(24);
            m.channel = r.get(16) as i32;
            m.service = r.get(16) as i32;
        }
        SRV_INFO => {
            if n < 48 {
                return false;
            }
            m.location = r.get(24);
            m.service = r.get(16) as i32;
        }
        _ => {}
    }
    true
}

/// Message type name ("other" when not one of the known types), NUL-terminated.
pub fn msg_name(t: i32) -> &'static str {
    match t {
        VCALL => "VCALL\0",
        TX_REL => "TX_REL\0",
        IDLE => "IDLE\0",
        SITE_INFO => "SITE_INFO\0",
        SRV_INFO => "SRV_INFO\0",
        STAT_REQ => "STAT_REQ\0",
        SD_HEADER => "SDCALL_HEADER\0",
        SD_DATA => "SDCALL_DATA\0",
        _ => "other\0",
    }
}

/// Call type name, NUL-terminated.
pub fn call_type_name(t: i32) -> &'static str {
    match t {
        0 => "broadcast\0",
        1 => "group\0",
        4 => "individual\0",
        6 => "interconnect\0",
        _ => "other\0",
    }
}

/// Spreads the 72 voice bits of a VCH into the mbelib `ambe_fr[4][24]` layout (96 bytes, row major).
pub fn ambe_frame(bits: &[u8; 72], fr: &mut [u8; 96]) {
    const NW: [u8; 36] = [0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2];
    const NX: [u8; 36] =
        [23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5, 17, 4, 16, 3, 15, 2, 14, 1, 13, 0, 12, 10, 11, 9, 10, 8, 9, 7, 8, 6, 7, 5, 6, 4];
    const NY: [u8; 36] = [0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3];
    const NZ: [u8; 36] =
        [5, 3, 4, 2, 3, 1, 2, 0, 1, 13, 0, 12, 22, 11, 21, 10, 20, 9, 19, 8, 18, 7, 17, 6, 16, 5, 15, 4, 14, 3, 13, 2, 12, 1, 11, 0];
    fr.iter_mut().for_each(|x| *x = 0);
    for j in 0..36 {
        fr[NW[j] as usize * 24 + NX[j] as usize] = bits[2 * j] & 1;
        fr[NY[j] as usize * 24 + NZ[j] as usize] = bits[2 * j + 1] & 1;
    }
}
