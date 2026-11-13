// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The fast access channel (ES 201 980 clause 6.3): 72 bits (116 in robustness mode E) with a CRC-8, the channel parameters and the service parameters of one
//! (two in mode E) services.
use crate::bits::{crc8, BitReader};

/// One set of service parameters (clause 6.3.4).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct FacService {
    /// 24 bit service identifier.
    pub id: u32,
    /// Short id.
    pub short_id: i32,
    /// Audio conditional access.
    pub audio_ca: u8,
    /// Data conditional access.
    pub data_ca: u8,
    /// Language (table 18).
    pub language: i32,
    /// Audio / data flag (1 = data).
    pub data: u8,
    /// Programme type or application identifier.
    pub descriptor: i32,
}

/// The FAC channel parameters.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct FacInfo {
    /// Base / enhancement flag.
    pub enhancement: u8,
    /// Robustness mode E (RM flag).
    pub mode_e: u8,
    /// Frame identity.
    pub identity: i32,
    /// Spectrum occupancy.
    pub occupancy: i32,
    /// Interleaver depth flag.
    pub interleaver: i32,
    /// MSC mode.
    pub msc_mode: i32,
    /// SDC mode.
    pub sdc_mode: i32,
    /// Number of services code.
    pub num_services_code: i32,
    /// Reconfiguration index.
    pub reconfig: i32,
    /// Toggle.
    pub toggle: i32,
    /// Service parameter sets (2 in mode E).
    pub n_svc: i32,
    /// The service parameters.
    pub svc: [FacService; 2],
}

/// Bits of a FAC block: 72, or 116 in mode E.
pub fn fac_info_bits(mode_e: bool) -> usize {
    if mode_e {
        116
    } else {
        72
    }
}

/// Parse a descrambled FAC block (`bits`: one bit per byte). `None` when the CRC is wrong or the RM flag disagrees with `mode_e`.
pub fn fac_parse(bits: &[u8], mode_e: bool) -> Option<FacInfo> {
    let total = fac_info_bits(mode_e);
    let body = total - 8;
    let mut tmp = [0u8; 120];
    for i in 0..body {
        tmp[i] = bits.get(i).copied().unwrap_or(0);
    }
    let mut cn = body;
    if mode_e {
        cn += 4;
    }
    let mut tail = BitReader::new(bits.get(body..).unwrap_or(&[]));
    if crc8(&tmp[..cn]) != tail.get(8) {
        return None;
    }
    let mut r = BitReader::new(bits);
    let mut f = FacInfo { enhancement: r.get(1) as u8, identity: r.get(2) as i32, mode_e: r.get(1) as u8, ..Default::default() };
    f.occupancy = r.get(3) as i32;
    f.interleaver = r.get(1) as i32;
    f.msc_mode = r.get(2) as i32;
    f.sdc_mode = r.get(1) as i32;
    f.num_services_code = r.get(4) as i32;
    f.reconfig = r.get(3) as i32;
    f.toggle = r.get(1) as i32;
    r.get(1);
    if (f.mode_e != 0) != mode_e {
        return None;
    }
    f.n_svc = if mode_e { 2 } else { 1 };
    for i in 0..f.n_svc as usize {
        let s = &mut f.svc[i];
        s.id = r.get(24);
        s.short_id = r.get(2) as i32;
        s.audio_ca = r.get(1) as u8;
        s.language = r.get(4) as i32;
        s.data = r.get(1) as u8;
        s.descriptor = r.get(5) as i32;
        s.data_ca = r.get(1) as u8;
        r.get(6);
    }
    Some(f)
}
