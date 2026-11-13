// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! P25 Phase 1 message fields (TIA-102.BAAC, BAAA): data unit ids and frame lengths, TSBK blocks (last block flag, protect flag, opcode, manufacturer id,
//! 64 bits of arguments, CRC-CCITT), link control words of LDU1 and TDULC (group / unit / terminator calls), the encryption sync of LDU2 and the header word
//! (message indicator, algorithm id, key id, manufacturer id, talkgroup), algorithm / opcode / data unit names and the channel number text.
use crate::trellis::crc_ccitt;

/// Header data unit.
pub const HDU: i32 = 0;
/// Simple terminator data unit.
pub const TDU: i32 = 3;
/// Logical link data unit 1 (voice + link control).
pub const LDU1: i32 = 5;
/// Trunking signalling block.
pub const TSBK: i32 = 7;
/// Logical link data unit 2 (voice + encryption sync).
pub const LDU2: i32 = 10;
/// Packet data unit.
pub const PDU: i32 = 12;
/// Terminator with link control.
pub const TDULC: i32 = 15;

/// Group voice channel grant.
pub const OP_GRP_V_GRANT: i32 = 0x00;
/// Group voice channel grant update.
pub const OP_GRP_V_GRANT_UPD: i32 = 0x02;
/// Unit to unit voice channel grant.
pub const OP_UU_V_GRANT: i32 = 0x04;
/// Identifier update for VHF / UHF.
pub const OP_IDEN_UP_VU: i32 = 0x34;
/// RFSS status broadcast.
pub const OP_RFSS_STS: i32 = 0x3A;
/// Network status broadcast.
pub const OP_NET_STS: i32 = 0x3B;
/// Adjacent site status broadcast.
pub const OP_ADJ_STS: i32 = 0x3C;
/// Identifier update.
pub const OP_IDEN_UP: i32 = 0x3D;

/// Group voice link control opcode.
pub const LC_GROUP: i32 = 0x00;
/// Unit to unit voice link control opcode.
pub const LC_UNIT: i32 = 0x03;
/// Call termination link control opcode.
pub const LC_TERM: i32 = 0x0F;

/// Data unit name, NUL-terminated.
pub fn duid_name(d: i32) -> &'static str {
    match d {
        HDU => "HDU\0",
        TDU => "TDU\0",
        LDU1 => "LDU1\0",
        TSBK => "TSBK\0",
        LDU2 => "LDU2\0",
        PDU => "PDU\0",
        TDULC => "TDULC\0",
        _ => "?\0",
    }
}

/// Frame length in dibits (status symbols included); TSBK / PDU: up to three blocks; 0 for an unknown unit.
pub fn frame_len(d: i32) -> i32 {
    match d {
        HDU => 396,
        TDU => 72,
        LDU1 | LDU2 => 864,
        TDULC => 216,
        TSBK | PDU => 360,
        _ => 0,
    }
}

/// Appends the low `n` bits of `v` (first bit on top) at `*p`.
fn put(b: &mut [u8], p: &mut usize, v: u64, n: u32) {
    for i in (0..n).rev() {
        b[*p] = ((v >> i) & 1) as u8;
        *p += 1;
    }
}
/// Reads `n` bits at `*p`.
fn get(b: &[u8], p: &mut usize, n: u32) -> u64 {
    let mut v = 0u64;
    for _ in 0..n {
        v = (v << 1) | u64::from(b[*p] & 1);
        *p += 1;
    }
    v
}
/// Six-bit groups (hexbits) of `n_hex * 6` bits.
fn bits_to_hex(b: &[u8], hex: &mut [u8]) {
    for (i, h) in hex.iter_mut().enumerate() {
        let mut v = 0u8;
        for k in 0..6 {
            v = (v << 1) | (b[6 * i + k] & 1);
        }
        *h = v;
    }
}
/// The inverse of [`bits_to_hex`].
fn hex_to_bits(hex: &[u8], b: &mut [u8]) {
    for (i, &h) in hex.iter().enumerate() {
        for k in 0..6 {
            b[6 * i + k] = (h >> (5 - k)) & 1;
        }
    }
}

/// One trunking signalling block.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Tsbk {
    /// Last block of the data unit.
    pub last: bool,
    /// Protected (encrypted) block.
    pub prot: bool,
    /// Opcode (6 bits).
    pub opcode: i32,
    /// Manufacturer id.
    pub mfid: i32,
    /// 64 bits of arguments (bit 63 sent first).
    pub args: u64,
}

impl Default for Tsbk {
    fn default() -> Self {
        Tsbk { last: true, prot: false, opcode: 0, mfid: 0, args: 0 }
    }
}

/// The 96 bits of a TSBK with its CRC.
pub fn tsbk_bits(t: &Tsbk, b: &mut [u8; 96]) {
    let mut p = 0;
    put(b, &mut p, u64::from(t.last), 1);
    put(b, &mut p, u64::from(t.prot), 1);
    put(b, &mut p, t.opcode as i64 as u64, 6);
    put(b, &mut p, t.mfid as i64 as u64, 8);
    put(b, &mut p, t.args, 64);
    let crc = crc_ccitt(&b[..80]);
    put(b, &mut p, u64::from(crc), 16);
}

/// Reads a TSBK from 96 bits; the second value is true when the CRC matches (the fields are filled either way).
pub fn tsbk_parse(b: &[u8; 96]) -> (Tsbk, bool) {
    let mut p = 0;
    let t = Tsbk {
        last: get(b, &mut p, 1) != 0,
        prot: get(b, &mut p, 1) != 0,
        opcode: get(b, &mut p, 6) as i32,
        mfid: get(b, &mut p, 8) as i32,
        args: get(b, &mut p, 64),
    };
    let crc = get(b, &mut p, 16) as u16;
    (t, crc == crc_ccitt(&b[..80]))
}

/// TSBK opcode name, NUL-terminated.
pub fn opcode_name(op: i32) -> &'static str {
    match op {
        OP_GRP_V_GRANT => "GRP_V_CH_GRANT\0",
        OP_GRP_V_GRANT_UPD => "GRP_V_CH_GRANT_UPDT\0",
        OP_UU_V_GRANT => "UU_V_CH_GRANT\0",
        OP_IDEN_UP_VU => "IDEN_UP_VU\0",
        OP_RFSS_STS => "RFSS_STS_BCST\0",
        OP_NET_STS => "NET_STS_BCST\0",
        OP_ADJ_STS => "ADJ_STS_BCST\0",
        OP_IDEN_UP => "IDEN_UP\0",
        _ => "TSBK\0",
    }
}

/// A link control word (72 bits of an LDU1 or TDULC).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Lc {
    /// Link control opcode.
    pub lco: i32,
    /// Manufacturer id.
    pub mfid: i32,
    /// Protected flag.
    pub prot: bool,
    /// Service options (0x80 emergency, 0x40 encrypted).
    pub svc_opts: i32,
    /// Talkgroup (group calls), -1 when absent.
    pub tg: i32,
    /// Source unit id, -1 when absent.
    pub src: i32,
    /// Destination unit id, -1 when absent.
    pub dst: i32,
    /// The low 64 of the 72 bits (bits 16..72 of the word, for opcodes not decoded here).
    pub raw: u64,
}

impl Default for Lc {
    fn default() -> Self {
        Lc { lco: 0, mfid: 0, prot: false, svc_opts: 0, tg: -1, src: -1, dst: -1, raw: 0 }
    }
}

/// The 12 hexbits of a link control word (group, unit to unit, terminator; any other opcode carries `raw`).
pub fn lc_hexbits(l: &Lc, hex: &mut [u8; 12]) {
    let mut b = [0u8; 72];
    let mut p = 0;
    put(&mut b, &mut p, u64::from(l.prot), 1);
    put(&mut b, &mut p, 0, 1);
    put(&mut b, &mut p, l.lco as i64 as u64, 6);
    put(&mut b, &mut p, l.mfid as i64 as u64, 8);
    if l.lco == LC_GROUP {
        put(&mut b, &mut p, l.svc_opts as i64 as u64, 8);
        put(&mut b, &mut p, 0, 8);
        put(&mut b, &mut p, l.tg as i64 as u64, 16);
        put(&mut b, &mut p, l.src as i64 as u64, 24);
    } else if l.lco == LC_UNIT {
        put(&mut b, &mut p, l.svc_opts as i64 as u64, 8);
        put(&mut b, &mut p, l.dst as i64 as u64, 24);
        put(&mut b, &mut p, l.src as i64 as u64, 24);
    } else if l.lco == LC_TERM {
        put(&mut b, &mut p, 0, 32);
        put(&mut b, &mut p, (if l.dst < 0 { 0 } else { l.dst }) as u64, 24);
    } else {
        put(&mut b, &mut p, l.raw & 0x00FF_FFFF_FFFF_FFFF, 56);
    }
    bits_to_hex(&b, hex);
}

/// Reads a link control word: group, unit and terminator fields for the standard and the 0x01 manufacturer ids, `raw` always.
pub fn lc_parse(hex: &[u8; 12]) -> Lc {
    let mut b = [0u8; 72];
    hex_to_bits(hex, &mut b);
    let mut p = 0;
    let mut l = Lc { prot: get(&b, &mut p, 1) != 0, ..Lc::default() };
    get(&b, &mut p, 1);
    l.lco = get(&b, &mut p, 6) as i32;
    l.mfid = get(&b, &mut p, 8) as i32;
    let mut q = p;
    l.raw = get(&b, &mut q, 56);
    if l.mfid != 0 && l.mfid != 1 {
        return l; // a manufacturer's own: kept raw
    }
    if l.lco == LC_GROUP {
        l.svc_opts = get(&b, &mut p, 8) as i32;
        get(&b, &mut p, 8);
        l.tg = get(&b, &mut p, 16) as i32;
        l.src = get(&b, &mut p, 24) as i32;
    } else if l.lco == LC_UNIT {
        l.svc_opts = get(&b, &mut p, 8) as i32;
        l.dst = get(&b, &mut p, 24) as i32;
        l.src = get(&b, &mut p, 24) as i32;
    } else if l.lco == LC_TERM {
        get(&b, &mut p, 32);
        l.dst = get(&b, &mut p, 24) as i32;
    }
    l
}

/// Encryption sync (LDU2) and header (HDU) fields.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Crypto {
    /// Message indicator (72 bits).
    pub mi: [u8; 9],
    /// Algorithm id (0x80 clear).
    pub alg_id: i32,
    /// Key id.
    pub key_id: i32,
    /// Manufacturer id (header only).
    pub mfid: i32,
    /// Talkgroup (header only), -1 when absent.
    pub tg: i32,
}

impl Default for Crypto {
    fn default() -> Self {
        Crypto { mi: [0; 9], alg_id: 0x80, key_id: 0, mfid: 0, tg: -1 }
    }
}

/// Algorithm name, NUL-terminated.
pub fn alg_name(a: i32) -> &'static str {
    match a {
        0x80 => "clear\0",
        0x81 => "DES-OFB\0",
        0x83 => "triple DES\0",
        0x84 => "AES-256\0",
        0x85 => "AES-128\0",
        0x9F => "DES-XL\0",
        0xAA => "ADP (RC4)\0",
        _ => "unknown algorithm\0",
    }
}

/// The 16 hexbits of an encryption sync.
pub fn es_hexbits(c: &Crypto, hex: &mut [u8; 16]) {
    let mut b = [0u8; 96];
    let mut p = 0;
    for &m in &c.mi {
        put(&mut b, &mut p, u64::from(m), 8);
    }
    put(&mut b, &mut p, c.alg_id as i64 as u64, 8);
    put(&mut b, &mut p, c.key_id as i64 as u64, 16);
    bits_to_hex(&b, hex);
}

/// Reads an encryption sync into `c` (message indicator, algorithm and key id only; the other fields are kept).
pub fn es_parse(hex: &[u8; 16], c: &mut Crypto) {
    let mut b = [0u8; 96];
    hex_to_bits(hex, &mut b);
    let mut p = 0;
    for m in c.mi.iter_mut() {
        *m = get(&b, &mut p, 8) as u8;
    }
    c.alg_id = get(&b, &mut p, 8) as i32;
    c.key_id = get(&b, &mut p, 16) as i32;
}

/// The 20 hexbits of a header word.
pub fn hdu_hexbits(c: &Crypto, hex: &mut [u8; 20]) {
    let mut b = [0u8; 120];
    let mut p = 0;
    for &m in &c.mi {
        put(&mut b, &mut p, u64::from(m), 8);
    }
    put(&mut b, &mut p, c.mfid as i64 as u64, 8);
    put(&mut b, &mut p, c.alg_id as i64 as u64, 8);
    put(&mut b, &mut p, c.key_id as i64 as u64, 16);
    put(&mut b, &mut p, (if c.tg < 0 { 0 } else { c.tg }) as u64, 16);
    bits_to_hex(&b, hex);
}

/// Reads a header word into `c`.
pub fn hdu_parse(hex: &[u8; 20], c: &mut Crypto) {
    let mut b = [0u8; 120];
    hex_to_bits(hex, &mut b);
    let mut p = 0;
    for m in c.mi.iter_mut() {
        *m = get(&b, &mut p, 8) as u8;
    }
    c.mfid = get(&b, &mut p, 8) as i32;
    c.alg_id = get(&b, &mut p, 8) as i32;
    c.key_id = get(&b, &mut p, 16) as i32;
    c.tg = get(&b, &mut p, 16) as i32;
}

/// A channel number (4-bit identifier, 12-bit channel) as "id-channel" in decimal; returns the length written to `out` (at most 8 bytes), or 0 when too small.
pub fn channel_text(ch: i32, out: &mut [u8]) -> usize {
    let mut tmp = [0u8; 16];
    let mut n = 0;
    for (i, v) in [((ch >> 12) & 15) as u32, (ch & 0xFFF) as u32].into_iter().enumerate() {
        if i == 1 {
            tmp[n] = b'-';
            n += 1;
        }
        let mut digits = [0u8; 5];
        let mut k = 0;
        let mut x = v;
        loop {
            digits[k] = b'0' + (x % 10) as u8;
            k += 1;
            x /= 10;
            if x == 0 {
                break;
            }
        }
        while k > 0 {
            k -= 1;
            tmp[n] = digits[k];
            n += 1;
        }
    }
    if out.len() < n {
        return 0;
    }
    out[..n].copy_from_slice(&tmp[..n]);
    n
}
