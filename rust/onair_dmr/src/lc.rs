// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! DMR link control and control blocks (ETSI TS 102 361-1 clause 7 and annex B): the full link control of voice calls (FLCO, FID, service options, destination
//! and source ids), header / terminator bursts (72 bit LC + Reed-Solomon (12,9) parity masked per data type), blocks of 96 bits that end in a CRC-CCITT
//! (CSBK, data headers, PI header, MBC) and the CSBK itself.
use crate::bits::{bits_to_bytes, Out};
use crate::codes::{crc_ccitt, rs129_correct, rs129_parity};
use crate::text::Buf;

/// Group voice call link control opcode.
pub const FLCO_GROUP_VOICE: i32 = 0;
/// Unit to unit voice call.
pub const FLCO_UNIT_VOICE: i32 = 3;
/// Talker alias header.
pub const FLCO_TA_HEADER: i32 = 4;
/// Talker alias block 1.
pub const FLCO_TA_BLOCK1: i32 = 5;
/// Talker alias block 2.
pub const FLCO_TA_BLOCK2: i32 = 6;
/// Talker alias block 3.
pub const FLCO_TA_BLOCK3: i32 = 7;
/// GPS info.
pub const FLCO_GPS_INFO: i32 = 8;

/// CSBK CRC mask (table B.x).
pub const MASK_CSBK: u16 = 0xA5A5;

/// 24-bit big endian value of three bytes.
pub fn be24(p: &[u8]) -> u32 {
    (u32::from(p[0]) << 16) | (u32::from(p[1]) << 8) | u32::from(p[2])
}
/// Writes the 24 low bits of `v` as three bytes, big endian.
pub fn put_be24(p: &mut [u8], v: u32) {
    p[0] = (v >> 16) as u8;
    p[1] = (v >> 8) as u8;
    p[2] = v as u8;
}

/// A full link control PDU (72 bits).
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct FullLc {
    /// Protect flag.
    pub pf: bool,
    /// Full link control opcode (6 bits).
    pub flco: i32,
    /// Feature set id.
    pub fid: i32,
    /// Service options (group and unit to unit calls).
    pub svc: u8,
    /// Destination (talkgroup or unit), 24 bits.
    pub dst: u32,
    /// Source, 24 bits.
    pub src: u32,
    /// The nine octets as received.
    pub raw: [u8; 9],
}

/// The nine octets of a full LC (PF + FLCO, FID, service options, destination, source).
pub fn pack_full_lc(lc: &FullLc) -> [u8; 9] {
    let mut out = [0u8; 9];
    out[0] = (if lc.pf { 0x80 } else { 0 }) | (lc.flco & 0x3F) as u8;
    out[1] = lc.fid as u8;
    out[2] = lc.svc;
    put_be24(&mut out[3..], lc.dst);
    put_be24(&mut out[6..], lc.src);
    out
}

/// Reads the nine octets of a full LC.
pub fn parse_full_lc(inp: &[u8; 9]) -> FullLc {
    FullLc {
        raw: *inp,
        pf: (inp[0] >> 7) & 1 != 0,
        flco: i32::from(inp[0] & 0x3F),
        fid: i32::from(inp[1]),
        svc: inp[2],
        dst: be24(&inp[3..]),
        src: be24(&inp[6..]),
    }
}

/// Name of a full LC opcode, NUL-terminated.
pub fn flco_name(f: i32) -> &'static str {
    match f {
        FLCO_GROUP_VOICE => "group voice\0",
        FLCO_UNIT_VOICE => "unit to unit voice\0",
        FLCO_TA_HEADER => "talker alias header\0",
        FLCO_TA_BLOCK1 | FLCO_TA_BLOCK2 | FLCO_TA_BLOCK3 => "talker alias block\0",
        FLCO_GPS_INFO => "GPS info\0",
        _ => "link control\0",
    }
}

/// Service options in words ("emergency privacy broadcast OVCM priority 2"); appended to `out`.
pub fn service_options_text(svc: u8, out: &mut Buf) {
    if svc & 0x80 != 0 {
        out.push_all(b"emergency ");
    }
    if svc & 0x40 != 0 {
        out.push_all(b"privacy ");
    }
    if svc & 0x08 != 0 {
        out.push_all(b"broadcast ");
    }
    if svc & 0x04 != 0 {
        out.push_all(b"OVCM ");
    }
    if svc & 3 != 0 {
        out.push_all(b"priority ");
        out.push(b'0' + (svc & 3));
        out.push(b' ');
    }
    out.pop_space(); // the one trailing space
}

/// Header / terminator burst information: 72 bit LC + RS (12,9) parity masked per data type (0x96 header, 0x99 terminator) as 96 bits.
pub fn lc_burst_info(lc: &[u8; 9], terminator: bool, info96: &mut [u8; 96]) {
    let mut w = [0u8; 12];
    w[..9].copy_from_slice(lc);
    let p = rs129_parity(lc);
    w[9..].copy_from_slice(&p);
    let mask = if terminator { 0x99 } else { 0x96 };
    for x in w.iter_mut().skip(9) {
        *x ^= mask;
    }
    Out::new(info96).put_bytes(&w);
}

/// Decodes a header / terminator burst: 0 clean, 1 one octet corrected, -1 failed (the nine octets are written in every case).
pub fn lc_burst_decode(info96: &[u8; 96], terminator: bool, lc: &mut [u8; 9]) -> i32 {
    let mut w = [0u8; 12];
    bits_to_bytes(info96, 0, 96, &mut w);
    let mask = if terminator { 0x99 } else { 0x96 };
    for x in w.iter_mut().skip(9) {
        *x ^= mask;
    }
    let e = rs129_correct(&mut w);
    lc.copy_from_slice(&w[..9]);
    e
}

/// 96 bits of a block: ten data octets and their CRC-CCITT XOR `mask`.
pub fn crc_block_info(data10: &[u8; 10], mask: u16, info96: &mut [u8; 96]) {
    let crc = crc_ccitt(data10) ^ mask;
    let mut o = Out::new(info96);
    o.put_bytes(data10);
    o.put(u64::from(crc), 16);
}

/// Checks the CRC of a 96 bit block; the ten data octets are written in every case.
pub fn crc_block_check(info96: &[u8; 96], mask: u16, data10: &mut [u8; 10]) -> bool {
    let mut b = [0u8; 12];
    bits_to_bytes(info96, 0, 96, &mut b);
    data10.copy_from_slice(&b[..10]);
    let crc = crc_ccitt(&b[..10]) ^ mask;
    crc == (u16::from(b[10]) << 8 | u16::from(b[11]))
}

/// A control signalling block.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Csbk {
    /// Last block.
    pub lb: bool,
    /// Protect flag.
    pub pf: bool,
    /// CSBK opcode (6 bits).
    pub opcode: i32,
    /// Feature set id.
    pub fid: i32,
    /// The eight argument octets.
    pub data: [u8; 8],
}

impl Default for Csbk {
    fn default() -> Self {
        Csbk { lb: true, pf: false, opcode: 0, fid: 0, data: [0; 8] }
    }
}

/// The 96 bits of a CSBK with its CRC.
pub fn csbk_info(c: &Csbk, info96: &mut [u8; 96]) {
    let mut d = [0u8; 10];
    d[0] = (if c.lb { 0x80 } else { 0 }) | (if c.pf { 0x40 } else { 0 }) | (c.opcode & 0x3F) as u8;
    d[1] = c.fid as u8;
    d[2..].copy_from_slice(&c.data);
    crc_block_info(&d, MASK_CSBK, info96);
}

/// Parses 96 bits as a CSBK; `None` when the CRC does not match.
pub fn csbk_parse(info96: &[u8; 96]) -> Option<Csbk> {
    let mut d = [0u8; 10];
    if !crc_block_check(info96, MASK_CSBK, &mut d) {
        return None;
    }
    let mut data = [0u8; 8];
    data.copy_from_slice(&d[2..]);
    Some(Csbk { lb: d[0] >> 7 != 0, pf: (d[0] >> 6) & 1 != 0, opcode: i32::from(d[0] & 0x3F), fid: i32::from(d[1]), data })
}

/// CSBK opcode name, NUL-terminated.
pub fn csbk_name(op: i32, fid: i32) -> &'static str {
    if fid != 0 {
        return "manufacturer CSBK\0";
    }
    match op {
        0x04 => "unit to unit voice request\0",
        0x05 => "unit to unit answer\0",
        0x07 => "channel timing\0",
        0x26 => "negative acknowledge\0",
        0x38 => "BS outbound activation\0",
        0x3D => "preamble\0",
        _ => "CSBK\0",
    }
}
