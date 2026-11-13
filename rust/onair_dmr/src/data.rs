// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! DMR data packets (ETSI TS 102 361-1 clause 9.3 and 9.2): the data header formats (unconfirmed / confirmed, response, short data defined and raw / status,
//! UDT, proprietary), the data block sizes, and encoding / parsing of the blocks of rate 1/2 (BPTC), rate 3/4 (trellis) and rate 1 data including the
//! confirmed data block serial number and CRC-9.
use crate::bits::Out;
use crate::codes::crc9;
use crate::lc::{be24, put_be24};
use crate::product::{bptc196_encode, rate1_encode, trellis34_encode};

/// Unified data transport.
pub const DPF_UDT: i32 = 0;
/// Response packet.
pub const DPF_RESPONSE: i32 = 1;
/// Unconfirmed data.
pub const DPF_UNCONFIRMED: i32 = 2;
/// Confirmed data.
pub const DPF_CONFIRMED: i32 = 3;
/// Short data, defined format.
pub const DPF_SHORT_DEFINED: i32 = 13;
/// Short data, raw or status / precoded.
pub const DPF_SHORT_RAW: i32 = 14;
/// Proprietary data.
pub const DPF_PROPRIETARY: i32 = 15;

/// Data types of the blocks (slot type).
pub const DT_RATE12: i32 = 7;
/// Rate 3/4 data.
pub const DT_RATE34: i32 = 8;
/// Rate 1 data.
pub const DT_RATE1: i32 = 10;

/// 9 bit CRC masks of the confirmed blocks.
const MASK_RATE12: u32 = 0x0F0;
const MASK_RATE34: u32 = 0x1FF;
const MASK_RATE1: u32 = 0x10F;

/// A data header (first ten octets of a header block).
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct DataHeader {
    /// Data packet format.
    pub dpf: i32,
    /// Service access point.
    pub sap: i32,
    /// Group (talkgroup) destination.
    pub group: bool,
    /// Response requested.
    pub resp: bool,
    /// Destination id.
    pub dst: u32,
    /// Source id.
    pub src: u32,
    /// Blocks to follow (BF), or appended blocks (AB) of the short data headers.
    pub blocks: i32,
    /// Pad octet count of a packet, pad bits of a short data message.
    pub pad: i32,
    /// Full message flag.
    pub fmf: bool,
    /// Fragment sequence number.
    pub fsn: i32,
    /// Send sequence number.
    pub ns: i32,
    /// Defined data format (short data).
    pub dd: i32,
    /// Source port.
    pub sp: i32,
    /// Destination port.
    pub dp: i32,
    /// Re-sync flag.
    pub resync: bool,
    /// Selective ARQ.
    pub sarq: bool,
    /// Status / precoded value.
    pub status: i32,
}

/// Name of a data packet format, NUL-terminated.
pub fn dpf_name(d: i32) -> &'static str {
    match d {
        DPF_UDT => "UDT\0",
        DPF_RESPONSE => "response\0",
        DPF_UNCONFIRMED => "unconfirmed data\0",
        DPF_CONFIRMED => "confirmed data\0",
        DPF_SHORT_DEFINED => "short data (defined)\0",
        DPF_SHORT_RAW => "short data (raw/status)\0",
        DPF_PROPRIETARY => "proprietary data\0",
        _ => "data\0",
    }
}

/// Name of a service access point, NUL-terminated.
pub fn sap_name(s: i32) -> &'static str {
    match s {
        0 => "UDT\0",
        2 => "TCP/IP header compression\0",
        3 => "UDP/IP header compression\0",
        4 => "IP\0",
        5 => "ARP\0",
        9 => "proprietary\0",
        10 => "short data\0",
        _ => "SAP\0",
    }
}

fn flag(b: bool, v: i32) -> i32 {
    if b {
        v
    } else {
        0
    }
}

/// The first ten octets of a header (the CRC is added by `crc_block_info` with the data header mask).
pub fn pack_data_header(h: &DataHeader) -> [u8; 10] {
    let mut o = [0u8; 10];
    let b = h.blocks;
    match h.dpf {
        DPF_UNCONFIRMED | DPF_CONFIRMED => {
            o[0] = (flag(h.group, 0x80) | flag(h.resp, 0x40) | (((h.pad >> 4) & 1) << 4) | h.dpf) as u8;
            o[1] = ((h.sap << 4) | (h.pad & 15)) as u8;
            put_be24(&mut o[2..], h.dst);
            put_be24(&mut o[5..], h.src);
            o[8] = (flag(h.fmf, 0x80) | (b & 0x7F)) as u8;
            o[9] =
                if h.dpf == DPF_CONFIRMED { (flag(h.resync, 0x80) | ((h.ns & 7) << 4) | (h.fsn & 15)) as u8 } else { (h.fsn & 15) as u8 };
        }
        DPF_SHORT_DEFINED => {
            o[0] = (flag(h.group, 0x80) | flag(h.resp, 0x40) | (((b >> 4) & 3) << 4) | h.dpf) as u8;
            o[1] = ((h.sap << 4) | (b & 15)) as u8;
            put_be24(&mut o[2..], h.dst);
            put_be24(&mut o[5..], h.src);
            o[8] = (((h.dd & 63) << 2) | flag(h.sarq, 2) | flag(h.fmf, 1)) as u8;
            o[9] = h.pad as u8;
        }
        DPF_SHORT_RAW => {
            o[0] = (flag(h.group, 0x80) | flag(h.resp, 0x40) | (((b >> 4) & 3) << 4) | h.dpf) as u8;
            o[1] = ((h.sap << 4) | (b & 15)) as u8;
            put_be24(&mut o[2..], h.dst);
            put_be24(&mut o[5..], h.src);
            if b == 0 {
                // status / precoded
                o[8] = (((h.sp & 7) << 5) | ((h.dp & 7) << 2) | ((h.status >> 8) & 3)) as u8;
                o[9] = (h.status & 0xFF) as u8;
            } else {
                o[8] = (((h.sp & 7) << 5) | ((h.dp & 7) << 2) | flag(h.sarq, 2) | flag(h.fmf, 1)) as u8;
                o[9] = h.pad as u8;
            }
        }
        _ => o[0] = h.dpf as u8,
    }
    o
}

/// Parses the ten octets of a header: (true, header), or (false, header with only the format set) for a format that is not known.
pub fn parse_data_header(i: &[u8; 10]) -> (bool, DataHeader) {
    let mut h = DataHeader { dpf: i32::from(i[0] & 15), ..DataHeader::default() };
    match h.dpf {
        DPF_UNCONFIRMED | DPF_CONFIRMED => {
            h.group = i[0] >> 7 != 0;
            h.resp = (i[0] >> 6) & 1 != 0;
            h.pad = (i32::from((i[0] >> 4) & 1) << 4) | i32::from(i[1] & 15);
            h.sap = i32::from(i[1] >> 4);
            h.dst = be24(&i[2..]);
            h.src = be24(&i[5..]);
            h.fmf = i[8] >> 7 != 0;
            h.blocks = i32::from(i[8] & 0x7F);
            if h.dpf == DPF_CONFIRMED {
                h.resync = i[9] >> 7 != 0;
                h.ns = i32::from((i[9] >> 4) & 7);
            }
            h.fsn = i32::from(i[9] & 15);
        }
        DPF_RESPONSE => {
            h.resp = (i[0] >> 6) & 1 != 0;
            h.sap = i32::from(i[1] >> 4);
            h.dst = be24(&i[2..]);
            h.src = be24(&i[5..]);
            h.fmf = i[8] >> 7 != 0;
            h.blocks = i32::from(i[8] & 0x7F);
            h.status = i32::from(i[9]);
        }
        DPF_SHORT_DEFINED => {
            h.group = i[0] >> 7 != 0;
            h.resp = (i[0] >> 6) & 1 != 0;
            h.blocks = (i32::from((i[0] >> 4) & 3) << 4) | i32::from(i[1] & 15);
            h.sap = i32::from(i[1] >> 4);
            h.dst = be24(&i[2..]);
            h.src = be24(&i[5..]);
            h.dd = i32::from(i[8] >> 2);
            h.sarq = (i[8] >> 1) & 1 != 0;
            h.fmf = i[8] & 1 != 0;
            h.pad = i32::from(i[9]);
        }
        DPF_SHORT_RAW => {
            h.group = i[0] >> 7 != 0;
            h.resp = (i[0] >> 6) & 1 != 0;
            h.blocks = (i32::from((i[0] >> 4) & 3) << 4) | i32::from(i[1] & 15);
            h.sap = i32::from(i[1] >> 4);
            h.dst = be24(&i[2..]);
            h.src = be24(&i[5..]);
            h.sp = i32::from(i[8] >> 5);
            h.dp = i32::from((i[8] >> 2) & 7);
            if h.blocks == 0 {
                h.status = (i32::from(i[8] & 3) << 8) | i32::from(i[9]);
            } else {
                h.sarq = (i[8] >> 1) & 1 != 0;
                h.fmf = i[8] & 1 != 0;
                h.pad = i32::from(i[9]);
            }
        }
        DPF_UDT => {
            h.group = i[0] >> 7 != 0;
            h.resp = (i[0] >> 6) & 1 != 0;
            h.sap = i32::from(i[1] >> 4);
            h.dst = be24(&i[2..]);
            h.src = be24(&i[5..]);
            h.blocks = i32::from(i[8] & 3);
        }
        DPF_PROPRIETARY => h.sap = i32::from(i[0] >> 4),
        _ => return (false, h),
    }
    (true, h)
}

/// Octets of a data block of type `dt`: 12 / 18 / 24 unconfirmed, 10 / 16 / 22 confirmed; 0 for other types.
pub fn data_block_bytes(dt: i32, confirmed: bool) -> i32 {
    match dt {
        DT_RATE12 => {
            if confirmed {
                10
            } else {
                12
            }
        }
        DT_RATE34 => {
            if confirmed {
                16
            } else {
                18
            }
        }
        DT_RATE1 => {
            if confirmed {
                22
            } else {
                24
            }
        }
        _ => 0,
    }
}

/// The 9 bit CRC mask of the confirmed blocks of data type `dt`.
fn confirmed_mask(dt: i32) -> u32 {
    if dt == DT_RATE34 {
        MASK_RATE34
    } else if dt == DT_RATE1 {
        MASK_RATE1
    } else {
        MASK_RATE12
    }
}

/// Encodes one data block of the 196 payload bits in transmit order: `user` is the data of the block (cut or zero-padded to the block size; confirmed blocks
/// carry the serial number `dbsn` and a CRC-9 in front); rate 3/4 uses the trellis, rate 1 direct bits, anything else BPTC.
pub fn data_block_encode(dt: i32, user: &[u8], confirmed: bool, dbsn: u32) -> [u8; 196] {
    let total: usize = if dt == DT_RATE12 {
        12
    } else if dt == DT_RATE34 {
        18
    } else {
        24
    };
    let mut oct = [0u8; 24];
    if confirmed {
        let ub = total - 2;
        let mut u = [0u8; 22];
        let n = user.len().min(ub);
        u[..n].copy_from_slice(&user[..n]);
        let crc = (u32::from(crc9(&u[..ub], dbsn)) ^ confirmed_mask(dt)) & 0x1FF;
        oct[0] = (((dbsn & 0x7F) << 1) | ((crc >> 8) & 1)) as u8;
        oct[1] = (crc & 0xFF) as u8;
        oct[2..2 + ub].copy_from_slice(&u[..ub]);
    } else {
        let n = user.len().min(total);
        oct[..n].copy_from_slice(&user[..n]);
    }
    if dt == DT_RATE34 {
        let mut d = [0u8; 18];
        d.copy_from_slice(&oct[..18]);
        trellis34_encode(&d)
    } else if dt == DT_RATE1 {
        rate1_encode(&oct)
    } else {
        let mut info = [0u8; 96];
        Out::new(&mut info).put_bytes(&oct[..12]);
        bptc196_encode(&info)
    }
}

/// A parsed data block.
pub struct DataBlock {
    /// Data octets (`len` valid).
    pub bytes: [u8; 24],
    /// Number of data octets.
    pub len: usize,
    /// Data block serial number (confirmed blocks).
    pub dbsn: u32,
    /// CRC-9 check of a confirmed block (true for unconfirmed).
    pub crc_ok: bool,
}

/// Splits the decoded octets of a block (`oct` holds the whole block, 12 / 18 / 24 octets by type); `None` for a type that is not a data block type.
pub fn data_block_parse(dt: i32, oct: &[u8], confirmed: bool) -> Option<DataBlock> {
    let total: usize = match dt {
        DT_RATE12 => 12,
        DT_RATE34 => 18,
        DT_RATE1 => 24,
        _ => return None,
    };
    if oct.len() < total {
        return None;
    }
    let mut b = DataBlock { bytes: [0; 24], len: total, dbsn: 0, crc_ok: true };
    if confirmed {
        b.dbsn = u32::from(oct[0] >> 1);
        let rx = (u32::from(oct[0] & 1) << 8) | u32::from(oct[1]);
        let calc = (u32::from(crc9(&oct[2..total], b.dbsn)) ^ confirmed_mask(dt)) & 0x1FF;
        b.crc_ok = rx == calc;
        b.len = total - 2;
        b.bytes[..total - 2].copy_from_slice(&oct[2..total]);
    } else {
        b.bytes[..total].copy_from_slice(&oct[..total]);
    }
    Some(b)
}
