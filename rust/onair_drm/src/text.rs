// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The DRM text message (ES 201 980 clause 6.5): segments of a 2 byte header, 1 to 16 bytes of text and a CRC-16, sent as 4 byte units, one per logical frame
//! (the reassembly of segments into the message stays in C++).
use onair_dab::crc::crc16;

/// Bytes a segment needs given its first byte: 2 header bytes, the text (a command has none) and 2 CRC bytes.
pub fn segment_need(b0: u8) -> i32 {
    let command = b0 & 0x10 != 0;
    let n = if command { 0 } else { (b0 & 15) as i32 + 1 };
    2 + n + 2
}

/// A checked text segment.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct TextSeg {
    /// The CRC-16 is right.
    pub crc_ok: u8,
    /// Toggle bit.
    pub toggle: u8,
    /// First segment of the message.
    pub first: u8,
    /// Last segment of the message.
    pub last: u8,
    /// A command (code in `cmd`), no text.
    pub command: u8,
    /// Command code (low 4 bits of byte 0).
    pub cmd: i32,
    /// Segment number (0 for the first).
    pub segnum: i32,
    /// Bytes of text (they start at byte 2).
    pub n: u32,
}

/// Check a segment of `need` bytes (`need = segment_need(seg[0])`, at least 4 bytes available).
pub fn segment_parse(seg: &[u8], need: i32) -> TextSeg {
    let mut r = TextSeg::default();
    if need < 4 || seg.len() < need as usize {
        return r;
    }
    let n = (need - 4) as usize;
    let rx = ((seg[2 + n] as u32) << 8) | seg[3 + n] as u32;
    if crc16(&seg[..2 + n]) as u32 != rx {
        return r;
    }
    r.crc_ok = 1;
    r.toggle = (seg[0] & 0x80 != 0) as u8;
    r.first = (seg[0] & 0x40 != 0) as u8;
    r.last = (seg[0] & 0x20 != 0) as u8;
    r.command = (seg[0] & 0x10 != 0) as u8;
    r.cmd = (seg[0] & 15) as i32;
    r.segnum = if r.first != 0 { 0 } else { ((seg[1] >> 4) & 7) as i32 };
    r.n = n as u32;
    r
}
