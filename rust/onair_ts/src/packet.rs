// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Transport packet header (ISO/IEC 13818-1 section 2.4.3.2) and the single-program PAT packet the service filter sends.
use crate::crc::mpeg_crc32;

/// The fields of a 188 byte packet header that the demultiplexer uses.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct PacketHeader {
    /// PID (13 bits).
    pub pid: i32,
    /// transport_error_indicator.
    pub tei: u8,
    /// payload_unit_start_indicator.
    pub pusi: u8,
    /// transport_scrambling_control.
    pub scr: i32,
    /// adaptation_field_control.
    pub afc: i32,
    /// continuity_counter.
    pub cc: i32,
    /// First payload byte, -1 when the packet has no payload or the adaptation field fills it.
    pub payload_off: i32,
}

/// Parse the header of a packet (at least 4 bytes, the first is 0x47); `None` when the sync byte is wrong or the packet is shorter than 4 bytes.
pub fn packet_header(p: &[u8]) -> Option<PacketHeader> {
    if p.len() < 4 || p[0] != 0x47 {
        return None;
    }
    let afc = ((p[3] >> 4) & 3) as i32;
    let mut off = -1;
    if afc & 1 != 0 {
        off = 4;
        if afc == 3 {
            off += 1 + p.get(4).copied().unwrap_or(0) as i32;
        }
        if off >= 188 {
            off = -1;
        }
    }
    Some(PacketHeader {
        pid: (((p[1] & 0x1F) as i32) << 8) | p[2] as i32,
        tei: p[1] >> 7,
        pusi: (p[1] >> 6) & 1,
        scr: ((p[3] >> 6) & 3) as i32,
        afc,
        cc: (p[3] & 15) as i32,
        payload_off: off,
    })
}

/// A 188 byte packet on PID 0 that carries one PAT section naming a single program (`sid` at `pmt_pid`) in transport stream `tsid`, with its CRC.
/// `cc` is the continuity counter to use (low 4 bits).
pub fn pat_packet(tsid: i32, sid: i32, pmt_pid: i32, cc: i32, out: &mut [u8; 188]) {
    let mut sec = [0u8; 16];
    sec[0] = 0x00;
    sec[1] = 0xB0;
    sec[2] = 13;
    sec[3] = (tsid >> 8) as u8;
    sec[4] = tsid as u8;
    sec[5] = 0xC1;
    sec[8] = (sid >> 8) as u8;
    sec[9] = sid as u8;
    sec[10] = (0xE0 | (pmt_pid >> 8)) as u8;
    sec[11] = pmt_pid as u8;
    let crc = mpeg_crc32(&sec[..12]);
    sec[12..16].copy_from_slice(&crc.to_be_bytes());
    out.fill(0xFF);
    out[0] = 0x47;
    out[1] = 0x40;
    out[2] = 0x00;
    out[3] = (0x10 | (cc & 15)) as u8;
    out[4] = 0x00;
    out[5..21].copy_from_slice(&sec);
}
